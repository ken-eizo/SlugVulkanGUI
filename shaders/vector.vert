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

layout(push_constant) uniform PushConstants {
  vec4 viewportScale;
  vec4 translationOverride;
  vec4 overrideClip;
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

void main() {
  const vec2 corners[6] = vec2[6](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
  vec2 corner = corners[gl_VertexIndex];
  vec2 positionPx = mix(inPositionRect.xy, inPositionRect.zw, corner);
  if (inShapeData.x != 0xFFFFFFFFu) positionPx.x += inPaint.w * (1.0 - corner.y);
  positionPx = positionPx * pushConstants.viewportScale.zw + pushConstants.translationOverride.xy;
  vec2 ndc = vec2(
    positionPx.x * 2.0 / pushConstants.viewportScale.x - 1.0,
    positionPx.y * 2.0 / pushConstants.viewportScale.y - 1.0
  );
  gl_Position = vec4(ndc, 0.0, 1.0);
  emCoord = mix(inEmRect.xy, inEmRect.zw, corner);
  uv = inShapeData.x == 0xFFFFFFFFu ? emCoord / max(inBandTransform.xy, vec2(0.0001)) : corner;
  bandTransform = inBandTransform;
  shapeData = inShapeData;
  color0 = inColor0;
  color1 = inColor1;
  paintData = inPaint;
  gradientData = inGradient;
  clipRect = pushConstants.translationOverride.z > 0.5 ? pushConstants.overrideClip : inClip;
}
