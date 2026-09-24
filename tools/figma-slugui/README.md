# SlugUI Exporter for Figma

This development plugin exports the current Figma selection as a typed `.slugui` component.
It has no network permission. Fill geometry is read in its documented node-local coordinates.
Center-aligned strokes use Figma's read-only `strokeGeometry`; Inside/Outside strokes use the
temporary exact `outlineStroke` result and remove it synchronously. The source selection is never
changed or reparented.

## Load locally

1. In the Figma desktop app, open **Plugins → Development → Import plugin from manifest**.
2. Select this directory's `manifest.json`.
3. Select a Frame, Component, Instance, or individual node and run **SlugUI Exporter**.
4. Download the generated `.slugui` file.
5. Compile it:

```bash
python tools/slugui_compiler.py MyComponent.slugui \
  --output generated/MyComponent.slugui.hpp \
  --namespace my_app::generated
```

The checked-in manifest ID is for local development. Replace it with the ID assigned by Figma
before publishing the plugin.

## Current semantic subset

- Frame/Component/ComponentSet/Instance/Group hierarchy
- component/instance provenance (`key`, main component, variant/property snapshots) in `source-provider-data`
- horizontal/vertical auto layout, absolute layout, absolute-in-auto-layout children, and min/max sizing
- fixed/hug/fill sizing, grow, padding, spacing, alignment, and clipping
- rectangle fill/stroke, uniform or top/right/bottom/left stroke weights, stroke alignment,
  independent corner radii, and corner smoothing
- solid, linear, radial, and diamond paint using the first/last gradient stops
- SVG-derived vector fill plus exact stroke geometry as typed `path-data`: CENTER reads
  `strokeGeometry` without mutation, while Inside/Outside uses Figma `outlineStroke()`
- text content, family, size, weight/style, decoration, alignment, line height, spacing, and indent
- stable Figma source metadata and explicit import fidelity
- dependency-aware live refresh on the current PageNode's `nodechange` event: selected/group/component
  edits re-export automatically, including hidden-child visibility and structural changes, while unrelated
  page edits are ignored
- dynamic-page component resolution through `getMainComponentAsync()`, with cached main-component trees
  for Instance provenance and dependency invalidation
- per-export memoization plus persistent vector/styled-text caches for repeated refreshes; current-page
  nodechange provides precise cache invalidation so unchanged SVG/text signatures are not rescanned
- the AOT compiler interns identical vector path geometry across component variants/instances, reducing
  path parsing, generated C++ size, C++ compile time, and VectorAtlas duplication

Vector paths are parsed by the AOT compiler and registered in `VectorAtlas` before it is built;
there is no SVG parser in the product runtime. Figma's center `strokeGeometry` and temporary
outline result resolve vector stroke alignment before AOT, so Inside/Center/Outside do not use a
runtime approximation. EVENODD
paths remain explicitly reported as approximated. Image/video/pattern/shader fills, effects,
multiple paints, variables, mixed-style text, angular gradients, and grid layout produce explicit
diagnostics. A vector node whose geometry is unavailable remains a typed `Shape` placeholder with
`E_VECTOR_GEOMETRY_MISSING` instead of silently claiming fidelity.
