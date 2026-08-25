#version 450

// Slug coverage solve derived from Eric Lengyel's public reference implementation.
// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2017 Eric Lengyel. Adapted for Vulkan/slughorn atlas textures.

layout(set = 0, binding = 0) uniform sampler2D curveTexture;
layout(set = 0, binding = 1) uniform usampler2D bandTexture;

layout(location = 0) in vec2 emCoord;
layout(location = 1) in vec2 uv;
layout(location = 2) flat in vec4 bandTransform;
layout(location = 3) flat in uvec4 shapeData;
layout(location = 4) flat in vec4 color0;
layout(location = 5) flat in vec4 color1;
layout(location = 6) flat in vec4 paintData;
layout(location = 7) flat in vec4 gradientData;
layout(location = 8) flat in vec4 clipRect;

layout(location = 0) out vec4 outColor;

const int indirectionSize = 32;
const uint analyticRoundedRectShape = 0xFFFFFFFFu;

float roundedRectCoverage(vec2 point, vec4 metrics) {
  vec2 size = max(metrics.xy, vec2(0.0001));
  vec2 halfSize = size * 0.5;
  float radius = clamp(metrics.z, 0.0, min(halfSize.x, halfSize.y));
  vec2 centered = abs(point - halfSize);
  if (radius <= 0.0001) {
    vec2 q = centered - halfSize;
    float distance = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
    float aa = max(fwidth(distance), 0.0001);
    return 1.0 - smoothstep(-aa, aa, distance);
  }
  vec2 corner = max(centered - (halfSize - vec2(radius)), 0.0) / radius;
  float exponent = mix(2.0, 5.0, clamp(metrics.w * 0.01, 0.0, 1.0));
  float implicitCurve = pow(corner.x, exponent) + pow(corner.y, exponent) - 1.0;
  float aa = max(fwidth(implicitCurve), 0.0001);
  return 1.0 - smoothstep(-aa, aa, implicitCurve);
}

uint calcRootCode(float y1, float y2, float y3) {
  uint i1 = floatBitsToUint(y1) >> 31u;
  uint i2 = floatBitsToUint(y2) >> 30u;
  uint i3 = floatBitsToUint(y3) >> 29u;
  uint shift = (i2 & 2u) | (i1 & ~2u);
  shift = (i3 & 4u) | (shift & ~4u);
  return (0x2E74u >> shift) & 0x0101u;
}

vec2 solveHorizontal(vec4 p12, vec2 p3) {
  vec2 a = p12.xy - p12.zw * 2.0 + p3;
  vec2 b = p12.xy - p12.zw;
  float d = sqrt(max(b.y * b.y - a.y * p12.y, 0.0));
  float t1;
  float t2;
  if (abs(a.y) < 1.0 / 65536.0) {
    t1 = abs(b.y) > 1.0 / 65536.0 ? p12.y * 0.5 / b.y : 0.0;
    t2 = t1;
  } else {
    t1 = (b.y - d) / a.y;
    t2 = (b.y + d) / a.y;
  }
  return vec2((a.x * t1 - b.x * 2.0) * t1 + p12.x,
              (a.x * t2 - b.x * 2.0) * t2 + p12.x);
}

vec2 solveVertical(vec4 p12, vec2 p3) {
  vec2 a = p12.xy - p12.zw * 2.0 + p3;
  vec2 b = p12.xy - p12.zw;
  float d = sqrt(max(b.x * b.x - a.x * p12.x, 0.0));
  float t1;
  float t2;
  if (abs(a.x) < 1.0 / 65536.0) {
    t1 = abs(b.x) > 1.0 / 65536.0 ? p12.x * 0.5 / b.x : 0.0;
    t2 = t1;
  } else {
    t1 = (b.x - d) / a.x;
    t2 = (b.x + d) / a.x;
  }
  return vec2((a.y * t1 - b.y * 2.0) * t1 + p12.y,
              (a.y * t2 - b.y * 2.0) * t2 + p12.y);
}

ivec2 bandLocation(ivec2 glyphLocation, uint offset) {
  int textureWidth = textureSize(bandTexture, 0).x;
  ivec2 location = ivec2(glyphLocation.x + int(offset), glyphLocation.y);
  location.y += location.x / textureWidth;
  location.x %= textureWidth;
  return location;
}

float slugCoverage(vec2 renderCoord) {
  vec2 emsPerPixel = max(fwidth(renderCoord), vec2(1.0 / 65536.0));
  vec2 pixelsPerEm = 1.0 / emsPerPixel;
  ivec2 glyphLocation = ivec2(shapeData.xy);
  ivec2 bandMaximum = ivec2(shapeData.zw);

  int qY = clamp(int(renderCoord.y * bandTransform.y + bandTransform.w), 0, indirectionSize - 1);
  int qX = clamp(int(renderCoord.x * bandTransform.x + bandTransform.z), 0, indirectionSize - 1);
  int bandY = int(texelFetch(bandTexture, glyphLocation + ivec2(qY, 0), 0).r);
  int bandX = int(texelFetch(bandTexture, glyphLocation + ivec2(indirectionSize + qX, 0), 0).r);

  float xCoverage = 0.0;
  float xWeight = 0.0;
  uvec2 horizontal = texelFetch(bandTexture, glyphLocation + ivec2(2 * indirectionSize + bandY, 0), 0).xy;
  ivec2 horizontalLocation = bandLocation(glyphLocation, horizontal.y);
  for (uint i = 0u; i < horizontal.x; ++i) {
    ivec2 curveLocation = ivec2(texelFetch(bandTexture, horizontalLocation + ivec2(int(i), 0), 0).xy);
    vec4 p12 = texelFetch(curveTexture, curveLocation, 0) - vec4(renderCoord, renderCoord);
    vec2 p3 = texelFetch(curveTexture, curveLocation + ivec2(1, 0), 0).xy - renderCoord;
    if (max(max(p12.x, p12.z), p3.x) * pixelsPerEm.x < -0.5) break;
    uint code = calcRootCode(p12.y, p12.w, p3.y);
    if (code != 0u) {
      vec2 root = solveHorizontal(p12, p3) * pixelsPerEm.x;
      if ((code & 1u) != 0u) {
        xCoverage += clamp(root.x + 0.5, 0.0, 1.0);
        xWeight = max(xWeight, clamp(1.0 - abs(root.x) * 2.0, 0.0, 1.0));
      }
      if (code > 1u) {
        xCoverage -= clamp(root.y + 0.5, 0.0, 1.0);
        xWeight = max(xWeight, clamp(1.0 - abs(root.y) * 2.0, 0.0, 1.0));
      }
    }
  }

  float yCoverage = 0.0;
  float yWeight = 0.0;
  uvec2 vertical = texelFetch(bandTexture,
    glyphLocation + ivec2(2 * indirectionSize + bandMaximum.y + 1 + bandX, 0), 0).xy;
  ivec2 verticalLocation = bandLocation(glyphLocation, vertical.y);
  for (uint i = 0u; i < vertical.x; ++i) {
    ivec2 curveLocation = ivec2(texelFetch(bandTexture, verticalLocation + ivec2(int(i), 0), 0).xy);
    vec4 p12 = texelFetch(curveTexture, curveLocation, 0) - vec4(renderCoord, renderCoord);
    vec2 p3 = texelFetch(curveTexture, curveLocation + ivec2(1, 0), 0).xy - renderCoord;
    if (max(max(p12.y, p12.w), p3.y) * pixelsPerEm.y < -0.5) break;
    uint code = calcRootCode(p12.x, p12.z, p3.x);
    if (code != 0u) {
      vec2 root = solveVertical(p12, p3) * pixelsPerEm.y;
      if ((code & 1u) != 0u) {
        yCoverage -= clamp(root.x + 0.5, 0.0, 1.0);
        yWeight = max(yWeight, clamp(1.0 - abs(root.x) * 2.0, 0.0, 1.0));
      }
      if (code > 1u) {
        yCoverage += clamp(root.y + 0.5, 0.0, 1.0);
        yWeight = max(yWeight, clamp(1.0 - abs(root.y) * 2.0, 0.0, 1.0));
      }
    }
  }
  float weighted = abs(xCoverage * xWeight + yCoverage * yWeight) / max(xWeight + yWeight, 1.0 / 65536.0);
  return clamp(max(weighted, min(abs(xCoverage), abs(yCoverage))), 0.0, 1.0);
}

vec4 evaluatePaint() {
  int kind = int(paintData.x + 0.5);
  float t = 0.0;
  vec2 origin = gradientData.xy;
  vec2 direction = gradientData.zw - origin;
  if (kind == 1) t = dot(uv - origin, direction) / max(dot(direction, direction), 0.00001);
  else if (kind == 2) {
    vec2 normalized = abs(uv - origin) / max(abs(direction), vec2(0.0001));
    t = normalized.x + normalized.y;
  } else if (kind == 3) t = length(uv - origin) / max(length(direction), 0.0001);
  else if (kind == 4) t = 0.5 + 0.5 * sin((uv.x * 1.7 + uv.y + paintData.z) * 24.0);
  return mix(color0, color1, clamp(t, 0.0, 1.0));
}

void main() {
  if (gl_FragCoord.x < clipRect.x || gl_FragCoord.y < clipRect.y ||
      gl_FragCoord.x >= clipRect.x + clipRect.z || gl_FragCoord.y >= clipRect.y + clipRect.w) discard;
  float coverage;
  if (shapeData.x == analyticRoundedRectShape) coverage = roundedRectCoverage(emCoord, bandTransform);
  else coverage = slugCoverage(emCoord);
  if (coverage <= 0.001) discard;
  vec4 color = evaluatePaint();
  color.a *= coverage * paintData.y;
  outColor = color;
}
