"use strict";

const FORMAT_VERSION = 3;
const diagnostics = [];
const fidelityCounts = { native: 0, approximated: 0, bakedVector: 0, unsupported: 0 };

function diagnostic(node, code, message, fidelity) {
  diagnostics.push({
    code,
    nodeId: node && node.id ? node.id : "",
    nodeName: node && node.name ? node.name : "",
    message,
    fidelity: fidelity || "Approximated",
  });
}

function isMixed(value) {
  return value === figma.mixed;
}

function finite(value, fallback) {
  return typeof value === "number" && Number.isFinite(value) ? value : fallback;
}

function number(value) {
  const rounded = Math.round(finite(value, 0) * 1000) / 1000;
  return String(Object.is(rounded, -0) ? 0 : rounded);
}

function slugString(value) {
  return JSON.stringify(String(value));
}

function identifier(value, fallback) {
  let result = String(value || fallback || "node")
    .normalize("NFKD")
    .replace(/[^A-Za-z0-9_]+/g, "_")
    .replace(/^_+|_+$/g, "");
  if (!result) result = fallback || "node";
  if (/^[0-9]/.test(result)) result = "_" + result;
  return result;
}

function elementName(node) {
  const id = String(node.id || "0").replace(/[^A-Za-z0-9]+/g, "_");
  return identifier(node.name, "node") + "_" + id;
}

function componentName(selection) {
  if (selection.length === 1) {
    const base = identifier(selection[0].name, "FigmaSelection");
    return base.charAt(0).toUpperCase() + base.slice(1) + "Generated";
  }
  return "FigmaSelectionGenerated";
}

function hexByte(value) {
  return Math.max(0, Math.min(255, Math.round(finite(value, 0) * 255)))
    .toString(16).padStart(2, "0");
}

function srgbDecode(value) {
  value = Math.max(0, finite(value, 0));
  return value <= 0.04045 ? value / 12.92 : Math.pow((value + 0.055) / 1.055, 2.4);
}

function srgbEncode(value) {
  value = Math.max(0, finite(value, 0));
  return value <= 0.0031308 ? value * 12.92 : 1.055 * Math.pow(value, 1 / 2.4) - 0.055;
}

function colorInSrgb(color) {
  const profile = figma.root && figma.root.documentColorProfile;
  if (profile !== "DISPLAY_P3") return color;
  const p3r = srgbDecode(color.r);
  const p3g = srgbDecode(color.g);
  const p3b = srgbDecode(color.b);
  return {
    r: srgbEncode(1.224940176 * p3r - 0.224940176 * p3g),
    g: srgbEncode(-0.042056955 * p3r + 1.042056955 * p3g),
    b: srgbEncode(-0.019637555 * p3r - 0.078636046 * p3g + 1.098273601 * p3b),
    a: color.a,
  };
}

function colorHex(color, opacity) {
  color = colorInSrgb(color);
  const alpha = finite(color.a, 1) * finite(opacity, 1);
  const rgb = "#" + hexByte(color.r) + hexByte(color.g) + hexByte(color.b);
  return alpha >= 0.9995 ? rgb : rgb + hexByte(alpha);
}

function visiblePaints(node) {
  if (!("fills" in node) || isMixed(node.fills) || !Array.isArray(node.fills)) {
    if ("fills" in node && isMixed(node.fills)) {
      diagnostic(node, "W_MIXED_FILL", "Mixed fills were replaced with transparent paint.");
    }
    return [];
  }
  return node.fills.filter((paint) => paint && paint.visible !== false);
}

function visibleStrokes(node) {
  if (!("strokes" in node) || isMixed(node.strokes) || !Array.isArray(node.strokes)) {
    if ("strokes" in node && isMixed(node.strokes)) {
      diagnostic(node, "W_MIXED_STROKE", "Mixed strokes were replaced with transparent paint.");
    }
    return [];
  }
  return node.strokes.filter((paint) => paint && paint.visible !== false);
}

function paintListSource(node, paints, role) {
  if (paints.length === 0) return "#00000000";
  if (paints.length > 1) {
    diagnostic(node, role === "fill" ? "W_MULTIPLE_FILLS" : "W_MULTIPLE_STROKES",
      "Only the first visible " + role + " is exported.");
  }
  const paint = paints[0];
  const opacity = finite(paint.opacity, 1) * finite(node.opacity, 1);
  if (paint.boundVariables && Object.keys(paint.boundVariables).length > 0) {
    diagnostic(node, "W_VARIABLE_BAKED", "Bound paint variables are exported as resolved values.");
  }
  if (paint.type === "SOLID") return colorHex(paint.color, opacity);
  if (paint.type && paint.type.indexOf("GRADIENT_") === 0) {
    const stops = Array.isArray(paint.gradientStops) ? paint.gradientStops : [];
    if (stops.length < 2) {
      diagnostic(node, "W_EMPTY_GRADIENT", "Gradient without two stops became transparent.");
      return "#00000000";
    }
    if (stops.length > 2) {
      diagnostic(node, "W_GRADIENT_STOPS", "SlugUI currently keeps the first and last gradient stops.");
    }
    if (paint.gradientTransform) {
      diagnostic(node, "W_GRADIENT_TRANSFORM", "Gradient transform is approximated by normalized endpoints.");
    }
    const first = colorHex(stops[0].color, opacity);
    const last = colorHex(stops[stops.length - 1].color, opacity);
    const names = {
      GRADIENT_LINEAR: "linear",
      GRADIENT_RADIAL: "radial",
      GRADIENT_DIAMOND: "diamond",
      GRADIENT_ANGULAR: "linear",
    };
    if (paint.type === "GRADIENT_ANGULAR") {
      diagnostic(node, "W_ANGULAR_GRADIENT", "Angular gradient is approximated as linear.");
    }
    return (names[paint.type] || "linear") + "(" + first + ", " + last + ")";
  }
  diagnostic(node, "E_UNSUPPORTED_PAINT",
    "Image, video, pattern, and Figma shader " + role + " paints require asset baking.",
    "Unsupported");
  return "#00000000";
}

function paintSource(node) {
  return paintListSource(node, visiblePaints(node), "fill");
}

function strokePaintSource(node) {
  return paintListSource(node, visibleStrokes(node), "stroke");
}

function hasVisualFill(node) {
  return visiblePaints(node).length > 0;
}

function hasVisualStroke(node) {
  return "strokes" in node && !isMixed(node.strokes) && Array.isArray(node.strokes) &&
    node.strokes.some((paint) => paint && paint.visible !== false);
}

function geometryPaths(node, property, fallbackProperty) {
  let paths = property in node && Array.isArray(node[property]) ? node[property] : [];
  if (paths.length === 0 && fallbackProperty &&
      fallbackProperty in node && Array.isArray(node[fallbackProperty])) {
    paths = node[fallbackProperty];
  }
  return paths.filter((path) => path && typeof path.data === "string" && path.data.trim() !== "");
}

function fillGeometryPaths(node) {
  return geometryPaths(node, "fillGeometry", "vectorPaths");
}

function nodeBounds(node, rendered) {
  const source = rendered && node.absoluteRenderBounds ? node.absoluteRenderBounds :
    (node.absoluteBoundingBox || null);
  if (source && [source.x, source.y, source.width, source.height].every(Number.isFinite)) {
    return { x: source.x, y: source.y, width: source.width, height: source.height };
  }
  return {
    x: finite(node.x, 0), y: finite(node.y, 0),
    width: finite(node.width, 0), height: finite(node.height, 0),
  };
}

function exportBounds(node) {
  const bounds = nodeBounds(node, false);
  if (nodeKind(node) !== "Shape" || (bounds.width > 0 && bounds.height > 0)) return bounds;
  const rendered = nodeBounds(node, true);
  return rendered.width > 0 && rendered.height > 0 ? rendered : bounds;
}

function removeTemporary(node) {
  if (!node || typeof node.remove !== "function") return;
  try { node.remove(); } catch (_) { /* the operation may already have consumed it */ }
}

function absoluteBounds(node, rendered) {
  const source = rendered && node.absoluteRenderBounds ? node.absoluteRenderBounds :
    (node.absoluteBoundingBox || null);
  if (source && [source.x, source.y, source.width, source.height].every(Number.isFinite)) {
    return { x: source.x, y: source.y, width: source.width, height: source.height };
  }
  return null;
}

function bakedVectorGeometry(node) {
  const result = { fillPaths: [], fillBounds: null, strokePaths: [], strokeBounds: null };
  if (hasVisualFill(node)) {
    try {
      // Figma guarantees fillGeometry is relative to the node. Reading it directly preserves the
      // local coordinate system; flattening a nested clone into the page changes its parent.
      result.fillPaths = fillGeometryPaths(node);
      result.fillBounds = nodeBounds(node, false);
    } catch (error) {
      diagnostic(node, "E_VECTOR_FLATTEN",
        "Figma could not read this vector fill: " + (error && error.message ? error.message : error),
        "Unsupported");
    }
  }
  if (hasVisualStroke(node)) {
    let outlined = null;
    try {
      if (typeof node.outlineStroke !== "function") {
        throw new Error("outlineStroke is unavailable");
      }
      // This is Figma's own exact Inside/Center/Outside result, not center geometry plus a
      // heuristic. The temporary outline is removed synchronously after reading its paths.
      outlined = node.outlineStroke();
      if (!outlined) throw new Error("outlineStroke returned no geometry");
      result.strokePaths = fillGeometryPaths(outlined);
      result.strokeBounds = absoluteBounds(node, true);
      if (result.strokePaths.length === 0) throw new Error("outlined stroke has no fillGeometry");
    } catch (error) {
      result.strokePaths = [];
      result.strokeBounds = null;
      diagnostic(node, "E_VECTOR_STROKE_OUTLINE",
        "Figma could not create the exact aligned stroke outline: " +
          (error && error.message ? error.message : error), "Unsupported");
    } finally {
      removeTemporary(outlined);
    }
  }
  return result;
}

function placementSource(bounds, ownerBounds) {
  if (!bounds || !ownerBounds || bounds.width <= 0 || bounds.height <= 0) return null;
  return "[" + [
    bounds.x - ownerBounds.x, bounds.y - ownerBounds.y, bounds.width, bounds.height,
  ].map((value) => number(value) + "px").join(", ") + "]";
}

function pathDataValue(paths) {
  const values = paths.map((path) => slugString(path.data));
  return values.length === 1 ? values[0] : "[" + values.join(", ") + "]";
}

function childrenOf(node) {
  return "children" in node && Array.isArray(node.children)
    ? node.children.filter((child) => child.visible !== false)
    : [];
}

function sceneOrderPath(node) {
  const result = [];
  let current = node;
  while (current && current.parent && Array.isArray(current.parent.children)) {
    const index = current.parent.children.indexOf(current);
    if (index < 0) return [];
    result.unshift(index);
    current = current.parent;
  }
  return result;
}

function compareSceneOrder(first, second) {
  const a = sceneOrderPath(first);
  const b = sceneOrderPath(second);
  const shared = Math.min(a.length, b.length);
  for (let index = 0; index < shared; ++index) {
    if (a[index] !== b[index]) return a[index] - b[index];
  }
  return a.length - b.length;
}

function topLevelSelection(selection) {
  const selected = new Set(selection);
  return selection.filter((node) => {
    let parent = node.parent;
    while (parent) {
      if (selected.has(parent)) return false;
      parent = parent.parent;
    }
    return true;
  });
}

function selectionKey(selection) {
  return selection.map((node) => String(node.id || "")).sort().join("|");
}

function containerLayout(node) {
  if (!("layoutMode" in node)) return "absolute";
  if (node.layoutMode === "HORIZONTAL") return "row";
  if (node.layoutMode === "VERTICAL") return "column";
  if (node.layoutMode === "GRID") {
    diagnostic(node, "W_GRID_LAYOUT", "Figma grid auto-layout is exported as Absolute.");
  }
  return "absolute";
}

function nodeKind(node) {
  if (node.type === "TEXT") return "Text";
  if (node.type === "RECTANGLE") return "Rectangle";
  if (["FRAME", "COMPONENT", "INSTANCE", "COMPONENT_SET", "GROUP", "SECTION"].includes(node.type)) {
    if (hasVisualFill(node) || hasVisualStroke(node) || "cornerRadius" in node) return "Rectangle";
    const layout = containerLayout(node);
    return layout === "row" ? "Row" : layout === "column" ? "Column" : "Absolute";
  }
  if (["VECTOR", "BOOLEAN_OPERATION", "ELLIPSE", "POLYGON", "STAR", "LINE"].includes(node.type)) {
    return "Shape";
  }
  return "Absolute";
}

function textListType(value) {
  if (!value || isMixed(value) || typeof value !== "object") return "";
  const type = String(value.type || "").toUpperCase();
  return ["ORDERED", "UNORDERED", "NONE"].includes(type) ? type : "";
}

function resolvedTextListType(node, segments) {
  const direct = textListType(node.listOptions);
  if (direct) return direct;
  const types = new Set((segments || []).map((segment) =>
    textListType(segment.listOptions)).filter(Boolean));
  return types.size === 1 ? Array.from(types)[0] : "";
}

function resolvedParagraphIndent(node, segments) {
  if (typeof node.paragraphIndent === "number") return node.paragraphIndent;
  const values = new Set((segments || [])
    .filter((segment) => typeof segment.paragraphIndent === "number")
    .map((segment) => segment.paragraphIndent));
  return values.size === 1 ? Array.from(values)[0] : null;
}

function styledTextNeedsApproximation(node, segments) {
  const listType = resolvedTextListType(node, segments);
  const paragraphIndent = resolvedParagraphIndent(node, segments);
  return (segments || []).some((segment) => {
    const segmentList = textListType(segment.listOptions);
    const openType = segment.openTypeFeatures;
    return finite(segment.paragraphSpacing, 0) !== 0 ||
      finite(segment.listSpacing, 0) !== 0 ||
      finite(segment.indentation, 0) !== 0 ||
      (segmentList && listType && segmentList !== listType) ||
      (typeof segment.paragraphIndent === "number" && paragraphIndent !== null &&
       segment.paragraphIndent !== paragraphIndent) ||
      (segment.textDecorationStyle &&
       !["SOLID", "NONE"].includes(String(segment.textDecorationStyle))) ||
      (openType && typeof openType === "object" &&
       Object.values(openType).some((value) => Boolean(value)));
  });
}

function fidelityFor(node, vectorGeometry, textSegments) {
  if (["VECTOR", "BOOLEAN_OPERATION", "ELLIPSE", "POLYGON", "STAR", "LINE"].includes(node.type)) {
    if ((hasVisualFill(node) && (!vectorGeometry || vectorGeometry.fillPaths.length === 0)) ||
        (hasVisualStroke(node) && (!vectorGeometry || vectorGeometry.strokePaths.length === 0))) {
      return "unsupported";
    }
    const paths = vectorGeometry.fillPaths.concat(vectorGeometry.strokePaths);
    return paths.some((path) => path.windingRule === "EVENODD")
      ? "approximated" : "baked-vector";
  }
  const styledText = node.type === "TEXT" && textSegments && textSegments.length > 0;
  if (styledText && styledTextNeedsApproximation(node, textSegments)) return "approximated";
  if (!styledText && "fills" in node && isMixed(node.fills)) return "approximated";
  const paints = styledText ? [] : visiblePaints(node);
  if (paints.length > 0) {
    const paint = paints[0];
    if (["IMAGE", "VIDEO", "PATTERN", "SHADER"].includes(paint.type)) return "unsupported";
    if (paint.boundVariables && Object.keys(paint.boundVariables).length > 0) return "approximated";
    if (paint.type === "GRADIENT_ANGULAR" || paint.gradientTransform ||
        (Array.isArray(paint.gradientStops) && paint.gradientStops.length !== 2)) {
      return "approximated";
    }
  }
  if (node.effects && !isMixed(node.effects) && node.effects.some((effect) => effect.visible !== false)) {
    return "approximated";
  }
  if (!styledText && node.type === "TEXT" &&
      (isMixed(node.fontName) || isMixed(node.fontSize) || isMixed(node.textDecoration))) {
    return "approximated";
  }
  if (finite(node.opacity, 1) < 0.9995 && childrenOf(node).length > 0) {
    return "approximated";
  }
  if (!styledText && visiblePaints(node).length > 1) return "approximated";
  return "native";
}

function push(lines, depth, text) {
  lines.push("  ".repeat(depth) + text);
}

function attribute(lines, depth, name, value) {
  push(lines, depth, name + ": " + value + ";");
}

function emitSourceMetadata(node, lines, depth, fidelity) {
  attribute(lines, depth, "source-provider", slugString("figma"));
  attribute(lines, depth, "source-document", slugString(figma.fileKey || "local-document"));
  attribute(lines, depth, "source-node", slugString(node.id || ""));
  attribute(lines, depth, "source-name", slugString(node.name || ""));
  attribute(lines, depth, "fidelity", fidelity);
}

function emitSizeAndPosition(node, kind, parentMode, isRoot, lines, depth, parentBounds, bounds) {
  if (!isRoot && parentMode === "NONE") {
    attribute(lines, depth, "x", number(bounds.x - finite(parentBounds && parentBounds.x, 0)) + "px");
    attribute(lines, depth, "y", number(bounds.y - finite(parentBounds && parentBounds.y, 0)) + "px");
  }
  const horizontal = "layoutSizingHorizontal" in node ? node.layoutSizingHorizontal : "FIXED";
  const vertical = "layoutSizingVertical" in node ? node.layoutSizingVertical : "FIXED";
  const renderedWidth = kind === "Shape" ? bounds.width : finite(node.width, bounds.width);
  const renderedHeight = kind === "Shape" ? bounds.height : finite(node.height, bounds.height);
  if (isRoot || horizontal === "FIXED") {
    attribute(lines, depth, "width", number(renderedWidth) + "px");
  } else if (horizontal === "FILL" && parentMode === "HORIZONTAL") {
    attribute(lines, depth, "grow", "1");
  }
  if (isRoot || vertical === "FIXED") {
    attribute(lines, depth, "height", number(renderedHeight) + "px");
  } else if (vertical === "FILL" && parentMode === "VERTICAL") {
    attribute(lines, depth, "grow", "1");
  }
}

function emitContainerLayout(node, kind, lines, depth) {
  const layout = containerLayout(node);
  if (kind === "Rectangle") attribute(lines, depth, "layout", layout);
  if (layout === "row" || layout === "column") {
    const padding = [
      finite(node.paddingLeft, 0),
      finite(node.paddingTop, 0),
      finite(node.paddingRight, 0),
      finite(node.paddingBottom, 0),
    ];
    if (padding.some((value) => value !== 0)) {
      attribute(lines, depth, "padding",
        "[" + padding.map((value) => number(value) + "px").join(", ") + "]");
    }
    if (typeof node.itemSpacing === "number") {
      attribute(lines, depth, "spacing", number(node.itemSpacing) + "px");
    } else if (node.itemSpacing === "AUTO") {
      attribute(lines, depth, "justify", "space-between");
    }
    const justify = { MIN: "start", CENTER: "center", MAX: "end", SPACE_BETWEEN: "space-between" };
    const align = { MIN: "start", CENTER: "center", MAX: "end", BASELINE: "start" };
    if (justify[node.primaryAxisAlignItems]) {
      attribute(lines, depth, "justify", justify[node.primaryAxisAlignItems]);
    }
    if (align[node.counterAxisAlignItems]) {
      attribute(lines, depth, "cross-align", align[node.counterAxisAlignItems]);
    }
  }
  if (node.clipsContent === true) attribute(lines, depth, "clip", "true");
}

function emitCorners(node, lines, depth) {
  if (!("cornerRadius" in node)) return;
  if (!isMixed(node.cornerRadius) && typeof node.cornerRadius === "number") {
    if (node.cornerRadius !== 0) attribute(lines, depth, "radius", number(node.cornerRadius));
  } else {
    const radii = [
      finite(node.topLeftRadius, 0),
      finite(node.topRightRadius, 0),
      finite(node.bottomRightRadius, 0),
      finite(node.bottomLeftRadius, 0),
    ];
    if (radii.some((radius) => radius !== 0)) {
      attribute(lines, depth, "radius", "[" + radii.map(number).join(", ") + "]");
    }
  }
  if (typeof node.cornerSmoothing === "number" && node.cornerSmoothing !== 0) {
    attribute(lines, depth, "smoothing", number(node.cornerSmoothing * 100) + "%");
  }
}

function emitStroke(node, lines, depth) {
  const strokes = visibleStrokes(node);
  if (strokes.length === 0) return;
  attribute(lines, depth, "stroke", strokePaintSource(node));

  let width = typeof node.strokeWeight === "number" ? node.strokeWeight : 1;
  if (isMixed(node.strokeWeight)) {
    const sides = [
      finite(node.strokeTopWeight, 0),
      finite(node.strokeRightWeight, 0),
      finite(node.strokeBottomWeight, 0),
      finite(node.strokeLeftWeight, 0),
    ];
    attribute(lines, depth, "stroke-width",
      "[" + sides.map(number).join(", ") + "]");
  } else {
    attribute(lines, depth, "stroke-width", number(Math.max(0, width)));
  }
  const align = { INSIDE: "inside", CENTER: "center", OUTSIDE: "outside" };
  attribute(lines, depth, "stroke-align", align[node.strokeAlign] || "center");
}

function inferredTextWeight(value, fallback) {
  if (typeof value.fontWeight === "number") return value.fontWeight;
  const style = String(value.fontName && value.fontName.style || "");
  if (/thin/i.test(style)) return 100;
  if (/extra.?light|ultra.?light/i.test(style)) return 200;
  if (/light/i.test(style)) return 300;
  if (/medium/i.test(style)) return 500;
  if (/semi.?bold|demi.?bold/i.test(style)) return 600;
  if (/extra.?bold|ultra.?bold/i.test(style)) return 800;
  if (/black|heavy/i.test(style)) return 900;
  if (/bold/i.test(style)) return 700;
  return fallback;
}

function textStyleItalic(value) {
  return value.fontStyle === "ITALIC" ||
    /italic|oblique/i.test(String(value.fontName && value.fontName.style || ""));
}

function textLetterSpacing(value, fontSize) {
  if (!value || isMixed(value)) return 0;
  const spacing = finite(value.value, 0);
  return value.unit === "PERCENT" ? fontSize * spacing / 100 : spacing;
}

function textLineHeight(value, fontSize) {
  if (!value || isMixed(value) || value.unit === "AUTO") return 1.25;
  if (value.unit === "PERCENT") return finite(value.value, 125) / 100;
  if (value.unit === "PIXELS" && fontSize > 0) return finite(value.value, fontSize) / fontSize;
  return 1.25;
}

function casedText(text, textCase) {
  if (textCase === "UPPER" || textCase === "SMALL_CAPS" ||
      textCase === "SMALL_CAPS_FORCED") return text.toUpperCase();
  if (textCase === "LOWER") return text.toLowerCase();
  if (textCase === "TITLE") {
    return text.replace(/(^|\s)(\S)/gu, (match, prefix, character) =>
      prefix + character.toUpperCase());
  }
  return text;
}

function styledTextSegments(node) {
  if (typeof node.getStyledTextSegments !== "function") return null;
  try {
    return node.getStyledTextSegments([
      "fontSize", "fontName", "fontWeight", "fontStyle", "textDecoration",
      "textDecorationStyle", "textCase", "lineHeight", "letterSpacing", "fills",
      "paragraphIndent", "paragraphSpacing", "listOptions", "listSpacing",
      "indentation", "openTypeFeatures",
    ]);
  } catch (error) {
    diagnostic(node, "E_STYLED_TEXT",
      "Figma could not expose styled text segments: " +
        (error && error.message ? error.message : error), "Unsupported");
    return null;
  }
}

function textRunSource(node, segment) {
  const fontSize = finite(segment.fontSize, finite(node.fontSize, 14));
  const fontName = segment.fontName && typeof segment.fontName.family === "string"
    ? segment.fontName.family : "system-ui";
  const weight = Math.max(1, Math.min(1000, inferredTextWeight(segment, 400)));
  const decoration = String(segment.textDecoration || "NONE");
  const paints = Array.isArray(segment.fills)
    ? segment.fills.filter((paint) => paint && paint.visible !== false)
    : visiblePaints(node);
  return "text-run(" + [
    slugString(casedText(String(segment.characters || ""), segment.textCase)),
    slugString(fontName),
    number(fontSize),
    number(weight),
    String(textStyleItalic(segment)),
    String(decoration === "UNDERLINE"),
    String(decoration === "STRIKETHROUGH"),
    number(textLetterSpacing(segment.letterSpacing, fontSize)),
    number(textLineHeight(segment.lineHeight, fontSize)),
    paintListSource(node, paints, "fill"),
  ].join(", ") + ")";
}

function emitText(node, lines, depth, segments) {
  const runOnlyStyle = segments && segments.some((segment) =>
    segment.textCase && segment.textCase !== "ORIGINAL");
  const rich = segments && segments.length > 0 &&
    (segments.length > 1 || isMixed(node.fontName) || isMixed(node.fontSize) ||
     isMixed(node.fontWeight) || isMixed(node.textDecoration) || isMixed(node.fills) ||
     isMixed(node.lineHeight) || isMixed(node.letterSpacing) || runOnlyStyle);
  const renderedText = rich
    ? segments.map((segment) =>
      casedText(String(segment.characters || ""), segment.textCase)).join("")
    : String(node.characters || "");
  attribute(lines, depth, "text", slugString(renderedText));
  let italic = false;
  let weight = inferredTextWeight(node, 400);
  if (typeof node.fontSize === "number") {
    attribute(lines, depth, "font-size", number(node.fontSize));
  } else if (isMixed(node.fontSize) && !rich) {
    diagnostic(node, "W_MIXED_TEXT", "Mixed text sizes are flattened to the first/default run.");
  }
  if (node.fontName && !isMixed(node.fontName) && typeof node.fontName.family === "string") {
    attribute(lines, depth, "font", slugString(node.fontName.family));
    const style = String(node.fontName.style || "");
    italic = /italic|oblique/i.test(style);
  } else if (isMixed(node.fontName) && !rich) {
    diagnostic(node, "W_MIXED_FONT", "Mixed font families are flattened to the default font.");
  }
  if (isMixed(node.fontWeight) && !rich) {
    diagnostic(node, "W_MIXED_FONT_WEIGHT",
      "Mixed text weights require separate text runs and currently use weight 400.");
  }
  attribute(lines, depth, "font-weight", number(Math.max(1, Math.min(1000, weight))));
  if (italic) attribute(lines, depth, "italic", "true");
  if (node.textDecoration === "UNDERLINE") attribute(lines, depth, "underline", "true");
  if (node.textDecoration === "STRIKETHROUGH") attribute(lines, depth, "strikethrough", "true");
  const alignment = {
    LEFT: "left", CENTER: "center", RIGHT: "right", JUSTIFIED: "justify",
  };
  if (alignment[node.textAlignHorizontal]) {
    attribute(lines, depth, "text-align", alignment[node.textAlignHorizontal]);
  }
  const verticalAlignment = { TOP: "top", CENTER: "center", BOTTOM: "bottom" };
  if (verticalAlignment[node.textAlignVertical]) {
    attribute(lines, depth, "text-align-vertical", verticalAlignment[node.textAlignVertical]);
  }
  if (node.lineHeight && !isMixed(node.lineHeight)) {
    if (node.lineHeight.unit === "PERCENT") {
      attribute(lines, depth, "line-height", number(node.lineHeight.value / 100));
    } else if (node.lineHeight.unit === "PIXELS" && typeof node.fontSize === "number" && node.fontSize > 0) {
      attribute(lines, depth, "line-height", number(node.lineHeight.value / node.fontSize));
    }
  }
  if (node.letterSpacing && !isMixed(node.letterSpacing)) {
    let spacing = finite(node.letterSpacing.value, 0);
    if (node.letterSpacing.unit === "PERCENT" && typeof node.fontSize === "number") {
      spacing = node.fontSize * spacing / 100;
    }
    if (spacing !== 0) attribute(lines, depth, "letter-spacing", number(spacing));
  }
  const paragraphIndent = resolvedParagraphIndent(node, segments);
  if (paragraphIndent !== null && paragraphIndent !== 0) {
    attribute(lines, depth, "indent", number(paragraphIndent));
  }
  const listType = resolvedTextListType(node, segments);
  if (listType === "UNORDERED") {
    attribute(lines, depth, "list-marker", "bullet");
  } else if (listType === "ORDERED") {
    attribute(lines, depth, "list-marker", "numbered");
  }
  if (rich && styledTextNeedsApproximation(node, segments)) {
    diagnostic(node, "W_RICH_TEXT_ADVANCED",
      "Paragraph spacing, nested lists, decoration variants, or OpenType overrides need a richer text layout service.");
  }
  const basePaint = rich && segments.length > 0
    ? paintListSource(node, Array.isArray(segments[0].fills)
      ? segments[0].fills.filter((paint) => paint && paint.visible !== false) : [], "fill")
    : paintSource(node);
  attribute(lines, depth, "fill", basePaint);
  if (rich) {
    attribute(lines, depth, "text-runs",
      "[" + segments.map((segment) => textRunSource(node, segment)).join(", ") + "]");
  }
}

function emitNode(node, parentMode, isRoot, lines, depth, parentBounds) {
  const kind = nodeKind(node);
  const vectorGeometry = kind === "Shape" ? bakedVectorGeometry(node) : null;
  const textSegments = kind === "Text" ? styledTextSegments(node) : null;
  const bounds = exportBounds(node);
  const fidelity = fidelityFor(node, vectorGeometry, textSegments);
  const fidelityCount = fidelity === "baked-vector" ? "bakedVector" : fidelity;
  fidelityCounts[fidelityCount] += 1;
  push(lines, depth, kind + " " + elementName(node) + " {");
  emitSizeAndPosition(node, kind, parentMode, isRoot, lines, depth + 1, parentBounds, bounds);

  if (kind === "Rectangle" || ["Row", "Column", "Absolute"].includes(kind)) {
    emitContainerLayout(node, kind, lines, depth + 1);
  }
  if (kind === "Rectangle") {
    attribute(lines, depth + 1, "fill", paintSource(node));
    emitCorners(node, lines, depth + 1);
    emitStroke(node, lines, depth + 1);
  } else if (kind === "Text") {
    emitText(node, lines, depth + 1, textSegments);
  } else if (kind === "Shape") {
    const ownerBounds = bounds;
    const fillPaths = vectorGeometry ? vectorGeometry.fillPaths : [];
    const strokePaths = vectorGeometry ? vectorGeometry.strokePaths : [];
    if (fillPaths.length > 0) {
      attribute(lines, depth + 1, "path-data", pathDataValue(fillPaths));
      const placement = placementSource(vectorGeometry.fillBounds, ownerBounds);
      if (placement) attribute(lines, depth + 1, "path-placement", placement);
      attribute(lines, depth + 1, "fill", paintSource(node));
    }
    if (strokePaths.length > 0) {
      attribute(lines, depth + 1, "stroke-path-data", pathDataValue(strokePaths));
      const placement = placementSource(vectorGeometry.strokeBounds, ownerBounds);
      if (placement) attribute(lines, depth + 1, "stroke-placement", placement);
      emitStroke(node, lines, depth + 1);
    }
    if (fillPaths.length === 0 && strokePaths.length === 0) {
      attribute(lines, depth + 1, "shape", "0");
      attribute(lines, depth + 1, "fill", paintSource(node));
      diagnostic(node, "E_VECTOR_GEOMETRY_MISSING",
        "Figma exposed no renderable fillGeometry or strokeGeometry; a placeholder was emitted.",
        "Unsupported");
    } else if (fillPaths.concat(strokePaths).some((path) => path.windingRule === "EVENODD")) {
      diagnostic(node, "W_EVENODD_PATH",
        "EVENODD geometry is imported with Slug's non-zero winding and may need contour reversal.");
    }
  }

  if (node.effects && !isMixed(node.effects) &&
      node.effects.some((effect) => effect.visible !== false)) {
    diagnostic(node, "W_EFFECTS", "Blur and shadow effects are not represented by the current IR.");
  }
  if (kind !== "Rectangle" && hasVisualStroke(node) &&
      (!vectorGeometry || vectorGeometry.strokePaths.length === 0)) {
    diagnostic(node, "W_STROKES",
      "Figma exposed no strokeGeometry, so this node's stroke paint cannot be rendered.");
  }
  if (finite(node.opacity, 1) < 0.9995 && childrenOf(node).length > 0) {
    diagnostic(node, "W_GROUP_OPACITY",
      "Container opacity is baked into its own fill but does not yet isolate descendant compositing.");
  }
  if (![
    "TEXT", "RECTANGLE", "FRAME", "COMPONENT", "INSTANCE", "COMPONENT_SET", "GROUP", "SECTION",
    "VECTOR", "BOOLEAN_OPERATION", "ELLIPSE", "POLYGON", "STAR", "LINE",
  ].includes(node.type)) {
    diagnostic(node, "E_NODE_TYPE", "Unsupported node type " + node.type + " became an Absolute container.", "Unsupported");
  }
  emitSourceMetadata(node, lines, depth + 1, fidelity);

  const layout = "layoutMode" in node ? node.layoutMode : "NONE";
  for (const child of childrenOf(node)) {
    emitNode(child, layout, false, lines, depth + 1, bounds);
  }
  push(lines, depth, "}");
}

function exportSelection(selection) {
  diagnostics.length = 0;
  fidelityCounts.native = 0;
  fidelityCounts.approximated = 0;
  fidelityCounts.bakedVector = 0;
  fidelityCounts.unsupported = 0;
  if (!selection || selection.length === 0) {
    return {
      source: "",
      filename: "",
      component: "",
      diagnostics: [{
        code: "E_NO_SELECTION", nodeId: "", nodeName: "",
        message: "Select one Frame, Component, or node to export.", fidelity: "Unsupported",
      }],
      fidelityCounts: { ...fidelityCounts },
    };
  }
  // PageNode.selection ordering is explicitly unspecified. Preserve Figma painter order for
  // overlapping multi-selection exports by sorting through each node's document-tree index path.
  selection = topLevelSelection(Array.from(selection));
  if (selection.length > 1) selection.sort(compareSceneOrder);
  const component = componentName(selection);
  const lines = [
    "// SlugUI exchange format " + FORMAT_VERSION + ", generated by the Figma selection exporter.",
    "component " + component + " {",
  ];
  const bounds = selection.map((node) => nodeBounds(node, true));
  const left = Math.min(...bounds.map((value) => value.x));
  const top = Math.min(...bounds.map((value) => value.y));
  const right = Math.max(...bounds.map((value) => value.x + value.width));
  const bottom = Math.max(...bounds.map((value) => value.y + value.height));
  push(lines, 1, "Absolute selection_root {");
  attribute(lines, 2, "width", number(right - left) + "px");
  attribute(lines, 2, "height", number(bottom - top) + "px");
  for (const node of selection) {
    emitNode(node, "NONE", false, lines, 2,
      { x: left, y: top, width: right - left, height: bottom - top });
  }
  push(lines, 1, "}");
  lines.push("}", "");
  return {
    source: lines.join("\n"),
    filename: component + ".slugui",
    component,
    diagnostics: diagnostics.slice(),
    fidelityCounts: { ...fidelityCounts },
  };
}

let lastSelectionKey = "";

function refreshSelection(force) {
  const selection = Array.from(figma.currentPage.selection || []);
  const key = selectionKey(selection);
  if (!force && key === lastSelectionKey) return;
  lastSelectionKey = key;
  try {
    const result = exportSelection(selection);
    figma.ui.postMessage({
      type: "export-result",
      ...result,
      selectionCount: selection.length,
      formatVersion: FORMAT_VERSION,
    });
  } catch (error) {
    figma.ui.postMessage({
      type: "export-result",
      source: "",
      filename: "",
      component: "",
      selectionCount: selection.length,
      formatVersion: FORMAT_VERSION,
      diagnostics: [{
        code: "E_EXPORT", nodeId: "", nodeName: "",
        message: error && error.message ? error.message : String(error),
        fidelity: "Unsupported",
      }],
      fidelityCounts: { native: 0, approximated: 0, bakedVector: 0, unsupported: 0 },
    });
  }
}

figma.showUI(__html__, { width: 580, height: 700, themeColors: true });
figma.ui.onmessage = (message) => {
  if (!message || message.type === "refresh") refreshSelection(true);
  if (message && message.type === "close") figma.closePlugin();
};
figma.on("selectionchange", () => refreshSelection(false));
refreshSelection(true);
