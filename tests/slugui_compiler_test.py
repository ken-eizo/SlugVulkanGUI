#!/usr/bin/env python3

import sys
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPOSITORY / "tools"))

import slugui_compiler as compiler  # noqa: E402


class SlugUiCompilerTest(unittest.TestCase):
    def test_example_is_deterministic_and_typed(self):
        source_path = REPOSITORY / "examples" / "slugui_demo.slugui"
        source = source_path.read_text(encoding="utf-8")
        first = compiler.compile_text(source, source_path.name, "slugvk::example")
        second = compiler.compile_text(source, source_path.name, "slugvk::example")
        self.assertEqual(first, second)
        self.assertIn("struct SlugUiDemoGenerated", first)
        self.assertIn("Built result;", first)
        self.assertIn('hashId("SlugUiDemoGenerated/root")', first)
        self.assertNotIn("SlugUiDemoGenerated//root", first)
        self.assertIn("Property<std::int64_t> clicks", first)
        self.assertIn("ValueSource<slugvk::Paint>", first)
        self.assertIn("callback_toggle_popup", first)
        self.assertIn("ImportFidelity::Native", first)

    def test_reference_type_mismatch_is_rejected(self):
        source = """
component Invalid {
  property float amount = 1;
  Text root {
    text: $amount;
  }
}
"""
        with self.assertRaisesRegex(compiler.CompileError, "expected string"):
            compiler.compile_text(source, "invalid.slugui", "test")

    def test_unknown_attribute_is_rejected(self):
        source = """
component Invalid {
  Rectangle root {
    magic-shadow: true;
  }
}
"""
        with self.assertRaisesRegex(compiler.CompileError, "not valid"):
            compiler.compile_text(source, "invalid.slugui", "test")

    def test_comments_gradients_and_individual_corners(self):
        source = """
component PaintSample {
  // Values remain compile-time constants.
  Rectangle root {
    fill: radial(#ffffff, #102040, 0.5, 0.5, 1, 0.5, 0.8);
    radius: [4, 8, 12, 16];
    smoothing: [0%, 25%, 50%, 100%];
  }
}
"""
        generated = compiler.compile_text(source, "paint.slugui", "test")
        self.assertIn("GradientKind::Radial", generated)
        self.assertIn("CornerRadii{4.0f, 8.0f, 12.0f, 16.0f}", generated)
        self.assertIn("CornerSmoothing{0.0f, 25.0f, 50.0f, 100.0f}", generated)

    def test_unicode_is_emitted_as_utf8_bytes(self):
        source = """
component UnicodeSample {
  Text root {
    text: "日本語 😀";
  }
}
"""
        generated = compiler.compile_text(source, "unicode.slugui", "test")
        self.assertNotIn(r"\ud83d", generated.lower())
        self.assertIn(r"\360\237\230\200", generated)

    def test_transparent_eight_digit_color_emits_valid_cpp_float(self):
        source = """
component Transparent {
  Rectangle root {
    fill: #00000000;
  }
}
"""
        generated = compiler.compile_text(source, "transparent.slugui", "test")
        self.assertIn("Color::fromRgb8(0x000000, 0.0f)", generated)
        self.assertNotIn(", 0f)", generated)

    def test_rectangle_stroke_paint_width_and_alignment_are_typed(self):
        source = """
component BorderSample {
  Rectangle root {
    fill: #101820;
    stroke: linear(#ff0080, #00e5ff);
    stroke-width: 2.5;
    stroke-align: inside;
  }
}
"""
        generated = compiler.compile_text(source, "border.slugui", "test")
        self.assertIn(".stroke.paint.normal = slugvk::Paint::gradient", generated)
        self.assertIn(".stroke.width = 2.5f", generated)
        self.assertIn(".stroke.align = slugvk::StrokeAlign::Inside", generated)

    def test_rectangle_individual_stroke_widths_are_typed(self):
        source = """
component BorderSides {
  Rectangle root {
    width: 80px;
    height: 40px;
    stroke: #ffffff;
    stroke-width: [1, 2, 3, 4];
    stroke-align: outside;
  }
}
"""
        generated = compiler.compile_text(source, "border-sides.slugui", "test")
        self.assertIn(
            "BorderWidths{1.0f, 2.0f, 3.0f, 4.0f}", generated)
        self.assertIn(".stroke.individualWidths", generated)
        self.assertIn("StrokeAlign::Outside", generated)

    def test_rectangle_individual_stroke_widths_can_bind(self):
        source = """
component BoundBorderSides {
  property border-widths sides = [1, 2, 3, 4];
  Rectangle root {
    stroke: #ffffff;
    stroke-width: $sides;
  }
}
"""
        generated = compiler.compile_text(source, "bound-border-sides.slugui", "test")
        self.assertIn("Property<slugvk::BorderWidths> sides", generated)
        self.assertIn("ValueSource<slugvk::BorderWidths>{result.sides}", generated)

    def test_svg_path_data_generates_prebuild_atlas_assets(self):
        source = """
component VectorAsset {
  Shape icon {
    width: 32px;
    height: 32px;
    path-data: ["M1 1 h20 v20 h-20 z", "M4 16 Q10 4 16 16 T28 16"];
    fill: #80d8ff;
    stroke-path-data: "M2 2 C8 0 20 0 26 2 S30 20 26 26 A4 4 0 0 1 22 30";
    stroke: #ff4080;
    stroke-width: 2;
    stroke-align: center;
  }
}
"""
        generated = compiler.compile_text(source, "vector.slugui", "test")
        self.assertIn("explicit VectorAsset(slugvk::VectorAtlas& atlas)", generated)
        self.assertGreaterEqual(generated.count("atlas->addPath"), 2)
        self.assertIn(".lineTo(", generated)
        self.assertIn(".quadraticTo(", generated)
        self.assertIn(".cubicTo(", generated)
        self.assertIn(".strokeShape = asset_", generated)
        self.assertIn(".fillPlacement = slugvk::Rect{", generated)
        self.assertIn(".strokePlacement = slugvk::Rect{", generated)
        self.assertNotIn("parse_svg", generated)
        self.assertRegex(generated, r"\.moveTo\(1\.0f, -1\.0f\)")

    def test_explicit_vector_placements_override_path_bounds(self):
        source = """
component PlacedVector {
  Shape icon {
    width: 20px;
    height: 10px;
    path-data: "M0 0 L4 0 L4 4 Z";
    path-placement: [2px, 1px, 8px, 6px];
    fill: #ffffff;
  }
}
"""
        generated = compiler.compile_text(source, "placed-vector.slugui", "test")
        self.assertIn("fillPlacement = slugvk::Rect{0.1f, 0.1f, 0.4f, 0.6f}", generated)

    def test_invalid_svg_path_is_rejected_at_aot_time(self):
        source = """
component InvalidVector {
  Shape icon {
    path-data: "M 0";
  }
}
"""
        with self.assertRaisesRegex(compiler.CompileError, "too few parameters"):
            compiler.compile_text(source, "invalid-vector.slugui", "test")

    def test_checked_in_figma_fixture_compiles(self):
        source_path = REPOSITORY / "examples" / "figma_group_31.slugui"
        generated = compiler.compile_text(
            source_path.read_text(encoding="utf-8"), source_path.name,
            "slugvk::example", "Group_31Generated")
        self.assertIn("struct Group_31Generated", generated)
        self.assertIn("slugvk::hashId(", generated)

    def test_generated_class_override_keeps_component_stable_ids(self):
        source = """
component FigmaSelectionGenerated {
  Absolute selection_root {
    Text label {
      text: "Weighted";
      font: "Geist";
      font-weight: 650;
    }
  }
}
"""
        generated = compiler.compile_text(
            source, "selection.slugui", "slugvk::example", "Group_31Generated")
        self.assertIn("struct Group_31Generated", generated)
        self.assertIn('hashId("FigmaSelectionGenerated/selection_root/label")', generated)
        self.assertIn(".style.weight = 650;", generated)

    def test_font_weight_range_is_checked(self):
        source = """
component InvalidWeight {
  Text label {
    text: "x";
    font-weight: 1200;
  }
}
"""
        with self.assertRaisesRegex(compiler.CompileError, "between 1 and 1000"):
            compiler.compile_text(source, "invalid-weight.slugui", "test")

    def test_mixed_text_runs_generate_typed_styles(self):
        source = """
component RichText {
  Text label {
    width: 200px;
    height: 40px;
    text: "Red blue";
    text-align: center;
    text-align-vertical: bottom;
    text-runs: [
      text-run("Red", "Geist", 16, 700, false, true, false, 0, 1.25, #ff0000),
      text-run(" blue", "Geist", 16, 400, true, false, false, 0.5, 1.25, #0000ff)
    ];
  }
}
"""
        generated = compiler.compile_text(source, "rich.slugui", "test")
        self.assertEqual(generated.count(".runs.push_back"), 2)
        self.assertIn('.fontName = "Geist"', generated)
        self.assertIn(".weight = 700", generated)
        self.assertIn(".weight = 400", generated)
        self.assertIn("VerticalAlign::Bottom", generated)
        self.assertIn("Color::fromRgb8(0xff0000)", generated)
        self.assertIn("Color::fromRgb8(0x0000ff)", generated)

    def test_mixed_text_runs_must_match_text(self):
        source = """
component InvalidRuns {
  Text label {
    text: "abc";
    text-runs: [text-run("ab", "Geist", 14, 400, false, false, false, 0, 1.25, #ffffff)];
  }
}
"""
        with self.assertRaisesRegex(compiler.CompileError, "exactly match"):
            compiler.compile_text(source, "invalid-runs.slugui", "test")

    def test_v1_figma_absolute_vector_placement_is_recentered(self):
        source = """
// SlugUI exchange format 1, generated by the Figma selection exporter.
component Legacy {
  Shape icon {
    width: 12px;
    height: 8px;
    stroke-path-data: "M0 0 L13.25 0 L13.25 9.25 Z";
    stroke-placement: [-284.625px, 227.375px, 13.25px, 9.25px];
    source-provider: "figma";
  }
}
"""
        generated = compiler.compile_text(source, "legacy.slugui", "test")
        self.assertIn(
            "strokePlacement = slugvk::Rect{-0.0520833333f, -0.078125f, "
            "1.10416667f, 1.15625f}", generated)

    def test_v2_disjoint_figma_vector_placement_is_recovered(self):
        source = """
// SlugUI exchange format 2, generated by the Figma selection exporter.
component BrokenV2 {
  Shape icon {
    width: 12px;
    height: 8px;
    stroke-path-data: "M0 0 L13.25 0 L13.25 9.25 Z";
    stroke-placement: [-284.625px, 227.375px, 13.25px, 9.25px];
    source-provider: "figma";
  }
}
"""
        generated = compiler.compile_text(source, "broken-v2.slugui", "test")
        self.assertIn(
            "strokePlacement = slugvk::Rect{-0.0520833333f, -0.078125f, "
            "1.10416667f, 1.15625f}", generated)

    def test_v2_local_figma_vector_placement_is_preserved(self):
        source = """
// SlugUI exchange format 2, generated by the Figma selection exporter.
component ValidV2 {
  Shape icon {
    width: 12px;
    height: 8px;
    stroke-path-data: "M0 0 L13.25 0 L13.25 9.25 Z";
    stroke-placement: [-0.625px, -0.625px, 13.25px, 9.25px];
    source-provider: "figma";
  }
}
"""
        generated = compiler.compile_text(source, "valid-v2.slugui", "test")
        self.assertIn(
            "strokePlacement = slugvk::Rect{-0.0520833333f, -0.078125f, "
            "1.10416667f, 1.15625f}", generated)

    def test_v2_zero_height_figma_line_recovers_render_bounds(self):
        source = """
// SlugUI exchange format 2, generated by the Figma selection exporter.
component FigmaLine {
  Shape divider {
    x: 10px;
    y: 20px;
    width: 23px;
    height: 0px;
    stroke-path-data: "M23 0 L23 6 L0 6 L0 0 L23 0 Z";
    stroke-placement: [0px, -3px, 23px, 6px];
    stroke-width: 6;
    stroke-align: outside;
    source-provider: "figma";
  }
}
"""
        generated = compiler.compile_text(source, "figma-line.slugui", "test")
        self.assertIn("strokePlacement = slugvk::Rect{0.0f, 0.0f, 1.0f, 1.0f}", generated)
        self.assertIn("layout.x = slugvk::slugui::Length::logical(10.0f)", generated)
        self.assertIn("layout.y = slugvk::slugui::Length::logical(17.0f)", generated)
        self.assertIn("layout.width = slugvk::slugui::Length::logical(23.0f)", generated)
        self.assertIn("layout.height = slugvk::slugui::Length::logical(6.0f)", generated)
        self.assertNotIn("sourceStroke", generated)


if __name__ == "__main__":
    unittest.main()
