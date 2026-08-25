#version 450

layout(location = 0) in vec2 inPositionPx;
layout(location = 1) in vec2 inEmCoord;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec4 inBandTransform;
layout(location = 4) in uvec4 inShapeData;
layout(location = 5) in vec4 inColor0;
layout(location = 6) in vec4 inColor1;
layout(location = 7) in vec4 inPaint;
layout(location = 8) in vec4 inGradient;
layout(location = 9) in vec4 inClip;

layout(push_constant) uniform PushConstants {
  vec2 viewport;
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
  vec2 ndc = vec2(
    inPositionPx.x * 2.0 / pushConstants.viewport.x - 1.0,
    inPositionPx.y * 2.0 / pushConstants.viewport.y - 1.0
  );
  gl_Position = vec4(ndc, 0.0, 1.0);
  emCoord = inEmCoord;
  uv = inUv;
  bandTransform = inBandTransform;
  shapeData = inShapeData;
  color0 = inColor0;
  color1 = inColor1;
  paintData = inPaint;
  gradientData = inGradient;
  clipRect = inClip;
}
