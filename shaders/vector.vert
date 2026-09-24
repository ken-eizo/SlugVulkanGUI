#version 450

layout(location = 0) in vec4 inPositionRect;
layout(location = 1) in vec4 inEmRect;
layout(location = 2) in vec4 inBandTransform;
layout(location = 3) in uvec4 inShapeData;
layout(location = 4) in vec4 inColor0;
layout(location = 5) in vec4 inColor1;
layout(location = 6) in vec4 inPaint;
layout(location = 7) in vec4 inGradient;
layout(location = 8) in vec4 inClip;
layout(location = 9) in vec4 inStrokeWidths;

layout(push_constant) uniform PushConstants {
  vec4 viewportScale;
  vec4 translationOverride;
  vec4 overrideClip;
  vec4 opacity;
} pushConstants;

layout(location = 0) out vec2 emCoord;
layout(location = 1) out vec2 uv;
layout(location = 2) flat out vec4 bandTransform;
layout(location = 3) flat out uvec4 shapeData;
layout(location = 4) flat out vec4 color0;
layout(location = 5) flat out vec4 color1;
layout(location = 6) flat out vec4 paintData;
layout(location = 7) flat out vec4 gradientData;
layout(location = 8) flat out vec4 clipRect;
layout(location = 9) flat out vec4 strokeWidths;

void main() {
  const uint externalImageShape = 0xFFFFFFFCu;
  const uint analyticArcShape = 0xFFFFFFFDu;
  const uint analyticStrokeSegmentShape = 0xFFFFFFFEu;
  const uint analyticRoundedRectShape = 0xFFFFFFFFu;
  const vec2 corners[6] = vec2[6](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
  vec2 corner = corners[gl_VertexIndex];
  vec2 authoredPosition = mix(inPositionRect.xy, inPositionRect.zw, corner);
  vec2 authoredEm = mix(inEmRect.xy, inEmRect.zw, corner);
  bool slugShape = inShapeData.x < externalImageShape;
  bool analyticShape = inShapeData.x >= analyticArcShape;

  // Slug's current reference implementation dynamically dilates every bounding polygon by
  // exactly half a viewport pixel. For our orthographic UI transform, the optimal displacement
  // reduces to +/-0.5 px on each adjacent edge. Move the em-space sample point through the
  // inverse affine Jacobian by the same amount so the outline itself does not grow.
  if (slugShape) {
    authoredPosition.x += inPaint.w * (1.0 - corner.y);

    vec2 retainedScale = pushConstants.viewportScale.zw;
    vec2 safeScale = vec2(
      abs(retainedScale.x) > 1.0e-8 ? retainedScale.x : 1.0,
      abs(retainedScale.y) > 1.0e-8 ? retainedScale.y : 1.0);
    vec2 outward = (corner * 2.0 - 1.0) * 0.5;

    vec2 emSpan = inEmRect.zw - inEmRect.xy;
    vec2 positionSpan = inPositionRect.zw - inPositionRect.xy;
    float emDy = outward.y * emSpan.y /
      max(abs(positionSpan.y * safeScale.y), 1.0e-8);
    // Artificial italic shear couples em-y motion into screen x. Remove that contribution before
    // mapping the remaining horizontal displacement back to em x.
    float shearScreenFromEmY = 0.0;
    if (abs(emSpan.y) > 1.0e-8)
      shearScreenFromEmY = safeScale.x * (-inPaint.w / emSpan.y) * emDy;
    float emDx = (outward.x - shearScreenFromEmY) * emSpan.x /
      max(abs(positionSpan.x * safeScale.x), 1.0e-8);

    emCoord = authoredEm + vec2(emDx, emDy);
    authoredPosition = authoredPosition * retainedScale +
      pushConstants.translationOverride.xy + outward;
  } else if (analyticShape) {
    // Analytic primitives follow the same half-device-pixel dilation invariant. Their
    // position/em mappings are affine and axis-aligned, so the inverse Jacobian is simply the
    // em-span divided by the scaled position-span. Keeping this in the vertex shader means a
    // retained primitive has identical AA at scale 0.5, 1, 2, ... instead of scaling a baked
    // logical-pixel fringe.
    vec2 retainedScale = pushConstants.viewportScale.zw;
    vec2 safeScale = vec2(
      abs(retainedScale.x) > 1.0e-8 ? retainedScale.x : 1.0,
      abs(retainedScale.y) > 1.0e-8 ? retainedScale.y : 1.0);
    vec2 outward = (corner * 2.0 - 1.0) * 0.5;
    vec2 emSpan = inEmRect.zw - inEmRect.xy;
    vec2 positionSpan = inPositionRect.zw - inPositionRect.xy;
    vec2 emDelta = outward * emSpan /
      max(abs(positionSpan * safeScale), vec2(1.0e-8));
    emCoord = authoredEm + emDelta;
    authoredPosition = authoredPosition * retainedScale +
      pushConstants.translationOverride.xy + outward;
  } else {
    emCoord = authoredEm;
    authoredPosition = authoredPosition * pushConstants.viewportScale.zw +
      pushConstants.translationOverride.xy;
  }

  vec2 ndc = vec2(
    authoredPosition.x * 2.0 / pushConstants.viewportScale.x - 1.0,
    authoredPosition.y * 2.0 / pushConstants.viewportScale.y - 1.0
  );
  gl_Position = vec4(ndc, 0.0, 1.0);
  uv = inShapeData.x == analyticRoundedRectShape
    ? emCoord / max(inBandTransform.xy, vec2(0.0001))
    : corner;
  bandTransform = inBandTransform;
  shapeData = inShapeData;
  color0 = inColor0;
  color1 = inColor1;
  paintData = inPaint;
  paintData.y *= pushConstants.opacity.x;
  gradientData = inGradient;
  clipRect = pushConstants.translationOverride.z > 0.5 ? pushConstants.overrideClip : inClip;
  strokeWidths = inStrokeWidths;
}
