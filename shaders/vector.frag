#version 450

// Slug coverage solve derived from Eric Lengyel's public reference implementation.
// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2017 Eric Lengyel. Adapted for Vulkan/slughorn atlas textures.

layout(set = 0, binding = 0) uniform sampler2D curveTexture;
layout(set = 0, binding = 1) uniform usampler2D bandTexture;

layout(push_constant) uniform PushConstants {
  vec4 viewportScale;
  vec4 translationOverride;
  vec4 overrideClip;
} pushConstants;

layout(location = 0) in vec2 emCoord;
layout(location = 1) in vec2 uv;
layout(location = 2) flat in vec4 bandTransform;
layout(location = 3) flat in uvec4 shapeData;
layout(location = 4) flat in vec4 color0;
layout(location = 5) flat in vec4 color1;
layout(location = 6) flat in vec4 paintData;
layout(location = 7) flat in vec4 gradientData;
layout(location = 8) flat in vec4 clipRect;
layout(location = 9) flat in vec4 strokeWidths;

layout(location = 0) out vec4 outColor;

const int indirectionSize = 32;
const uint analyticRoundedRectShape = 0xFFFFFFFFu;
const uint analyticStrokeSegmentShape = 0xFFFFFFFEu;

vec2 unpackFixed16(uint packed, float scale) {
  return vec2(float(packed & 0xFFFFu), float(packed >> 16u)) / scale;
}

float roundedRectShapeCoverage(vec2 point, vec2 size, vec4 radiiX, vec4 radiiY,
                               vec4 smoothing) {
  size = max(size, vec2(0.0001));
  vec2 halfSize = size * 0.5;
  int cornerIndex = point.y < halfSize.y
    ? (point.x < halfSize.x ? 0 : 1)
    : (point.x < halfSize.x ? 3 : 2);
  vec2 radius = max(vec2(radiiX[cornerIndex], radiiY[cornerIndex]), vec2(0.0));
  float exponent = mix(2.0, 5.0, clamp(smoothing[cornerIndex] * 0.01, 0.0, 1.0));
  vec2 fromCenter = abs(point - halfSize);
  vec2 boxDistanceVector = fromCenter - halfSize;
  if (max(radius.x, radius.y) <= 0.0001) {
    float distance = max(boxDistanceVector.x, boxDistanceVector.y);
    float aa = max(fwidth(distance), 0.0001);
    return clamp(0.5 - distance / aa, 0.0, 1.0);
  }

  // The rounded corner is a superellipse attached to the two straight edges. Convert its
  // implicit value to pixel distance with the analytic gradient. This is important even far
  // from a corner: treating the whole radius-wide edge strip as distance zero leaves a broad
  // 50% coverage band and makes an inside stroke look thick, translucent, and doubled.
  vec2 safeRadius = max(radius, vec2(0.0001));
  vec2 cornerOffset = fromCenter - (halfSize - safeRadius);
  vec2 positiveOffset = max(cornerOffset, vec2(0.0));
  float distance;
  if (max(positiveOffset.x, positiveOffset.y) <= 0.0001) {
    distance = max(boxDistanceVector.x, boxDistanceVector.y);
  } else {
    vec2 normalized = positiveOffset / safeRadius;
    vec2 poweredMinusOne = pow(normalized, vec2(exponent - 1.0));
    vec2 powered = poweredMinusOne * normalized;
    float powerSum = max(powered.x + powered.y, 1.0e-12);
    float inverseExponent = 1.0 / exponent;
    float implicitValue = pow(powerSum, inverseExponent);
    vec2 gradient = poweredMinusOne / safeRadius;
    gradient *= implicitValue / powerSum;
    distance = (implicitValue - 1.0) / max(length(gradient), 0.0001);
  }
  float aa = max(fwidth(distance), 0.0001);
  return clamp(0.5 - distance / aa, 0.0, 1.0);
}

float roundedRectCoverage(vec2 point, vec4 metrics) {
  vec2 size = max(metrics.xy, vec2(0.0001));
  float coverageMode = metrics.z;
  vec4 radii = vec4(unpackFixed16(shapeData.y, 16.0), unpackFixed16(shapeData.z, 16.0));
  vec4 smoothing = vec4(unpackFixed16(shapeData.w, 256.0),
                        unpackFixed16(floatBitsToUint(paintData.w), 256.0));
  vec4 widths = max(strokeWidths, vec4(0.0)); // top, right, bottom, left
  float outsideFactor = clamp(metrics.w, 0.0, 1.0);
  vec4 horizontal = vec4(widths.w, widths.y, widths.y, widths.w);
  vec4 vertical = vec4(widths.x, widths.x, widths.z, widths.z);
  vec4 innerRadiiX = max(radii - horizontal * (1.0 - outsideFactor), vec4(0.0));
  vec4 innerRadiiY = max(radii - vertical * (1.0 - outsideFactor), vec4(0.0));
  // An opaque Figma-style stroke is composited as an outer border mask followed by an inner
  // fill mask. The fill uses only the inward part of each independently authored side width.
  // Resolve this mode before evaluating the unrelated outer SDF.
  if (coverageMode < -0.5) {
    vec4 insets = widths * (1.0 - outsideFactor); // top, right, bottom, left
    vec2 innerSize = size - vec2(insets.w + insets.y, insets.x + insets.z);
    if (min(innerSize.x, innerSize.y) <= 0.0) return 0.0;
    return roundedRectShapeCoverage(
      point - vec2(insets.w, insets.x), innerSize, innerRadiiX, innerRadiiY, smoothing);
  }

  // A zero authored radius is a mitered rectangle corner, not a round cap produced by the
  // outside stroke width. Non-zero corners expand elliptically per adjacent side.
  vec4 roundedCorner = step(vec4(0.0001), radii);
  vec4 outerRadiiX = (radii + horizontal * outsideFactor) * roundedCorner;
  vec4 outerRadiiY = (radii + vertical * outsideFactor) * roundedCorner;
  float outer = roundedRectShapeCoverage(
    point, size, outerRadiiX, outerRadiiY, smoothing);
  if (max(max(widths.x, widths.y), max(widths.z, widths.w)) <= 0.0001) return outer;
  vec2 innerSize = size - vec2(widths.w + widths.y, widths.x + widths.z);
  if (min(innerSize.x, innerSize.y) <= 0.0) return outer;
  float inner = roundedRectShapeCoverage(
    point - vec2(widths.w, widths.x), innerSize, innerRadiiX, innerRadiiY, smoothing);
  float ring = clamp(outer - inner, 0.0, 1.0);
  // A fully opaque border stays solid beneath the inner fill's AA transition. On a zero-width
  // side outer == inner, so no border is introduced there. This applies outer coverage once and
  // prevents a lighter fill layer from leaking through the outside edge.
  if (coverageMode > 1.5) return ring > 0.00001 ? outer : 0.0;
  return ring;
}

float strokeSegmentCoverage(vec2 point, vec4 endpoints) {
  vec2 from = endpoints.xy;
  vec2 to = endpoints.zw;
  vec2 axis = to - from;
  float axisLength = max(length(axis), 0.0001);
  vec2 tangent = axis / axisLength;
  vec2 normal = vec2(-tangent.y, tangent.x);
  vec2 local = vec2(dot(point - from, tangent), dot(point - from, normal));
  float halfWidth = max(paintData.w * 0.5, 0.0001);
  bool roundStart = (shapeData.y & 1u) != 0u;
  bool roundEnd = (shapeData.y & 2u) != 0u;

  float distance;
  if (local.x < 0.0 && roundStart) {
    distance = length(local) - halfWidth;
  } else if (local.x > axisLength && roundEnd) {
    distance = length(vec2(local.x - axisLength, local.y)) - halfWidth;
  } else {
    vec2 q = vec2(max(max(-local.x, local.x - axisLength), 0.0),
                  abs(local.y) - halfWidth);
    distance = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
  }
  float aa = max(fwidth(distance), 0.0001);
  return 1.0 - smoothstep(-aa, aa, distance);
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
  else if (kind == 5) {
    vec2 centered = uv - origin;
    float hue = fract(atan(centered.x, -centered.y) / 6.28318530718 + 1.0);
    vec3 rgb = clamp(abs(mod(hue * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0,
                     0.0, 1.0);
    return vec4(rgb, color0.a);
  }
  return mix(color0, color1, clamp(t, 0.0, 1.0));
}

vec3 srgbToLinear(vec3 value) {
  bvec3 low = lessThanEqual(value, vec3(0.04045));
  vec3 linearLow = value / 12.92;
  vec3 linearHigh = pow((value + 0.055) / 1.055, vec3(2.4));
  return mix(linearHigh, linearLow, low);
}

void main() {
  if (gl_FragCoord.x < clipRect.x || gl_FragCoord.y < clipRect.y ||
      gl_FragCoord.x >= clipRect.x + clipRect.z || gl_FragCoord.y >= clipRect.y + clipRect.w) discard;
  float coverage;
  if (shapeData.x == analyticRoundedRectShape)
    coverage = roundedRectCoverage(emCoord, bandTransform);
  else if (shapeData.x == analyticStrokeSegmentShape)
    coverage = strokeSegmentCoverage(emCoord, bandTransform);
  else
    coverage = slugCoverage(emCoord);
  if (coverage <= 0.001) discard;
  vec4 color = evaluatePaint();
  if (pushConstants.translationOverride.w > 0.5)
    color.rgb = srgbToLinear(clamp(color.rgb, 0.0, 1.0));
  color.a *= coverage * paintData.y;
  outColor = color;
}
