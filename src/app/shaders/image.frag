// SPDX-License-Identifier: GPL-3.0-or-later
#version 440

// Image tile: high quality sampling (F-VIEW-06), color conversion into the output
// color space (F-COL-01, see ColorPipeline.h) and compositing over the background
// or the transparency checkerboard (F-VIEW-09). Output is opaque.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    mat4 colorMatrix;   // upper 3x3: linear source -> linear output
    vec4 uvRect;
    vec4 texInfo;       // xy: texture size in texels, z: 1 = supersampled minification
    vec4 transfer;      // x: source kind, y: source gamma, z: output kind, w: output gamma
    vec4 flags;         // x: premultiplied, y: identity color, z: checker, w: checker cell (px)
    vec4 bgColor;       // background (and first checker color), output encoded
    vec4 checkerColor;  // second checker color
    vec4 range;         // x: exposure multiplier (linear), y: max linear value (1 = SDR)
} ubuf;

layout(binding = 1) uniform sampler2D tex;
layout(binding = 2) uniform sampler2D srcLut;
layout(binding = 3) uniform sampler2D dstLut;

// Kinds: 0 linear, 1 sRGB, 2 gamma, 3 lookup table (see TransferKind).

vec3 srgbToLinear(vec3 c)
{
    vec3 a = abs(c);
    vec3 lo = a / 12.92;
    vec3 hi = pow((a + 0.055) / 1.055, vec3(2.4));
    return sign(c) * mix(hi, lo, vec3(lessThanEqual(a, vec3(0.04045))));
}

vec3 linearToSrgb(vec3 c)
{
    vec3 a = abs(c);
    vec3 lo = a * 12.92;
    vec3 hi = 1.055 * pow(a, vec3(1.0 / 2.4)) - 0.055;
    return sign(c) * mix(hi, lo, vec3(lessThanEqual(a, vec3(0.0031308))));
}

vec3 decodeTransfer(vec3 c)
{
    float kind = ubuf.transfer.x;
    if (kind < 0.5)
        return c;
    if (kind < 1.5)
        return srgbToLinear(c);
    if (kind < 2.5)
        return sign(c) * pow(abs(c), vec3(ubuf.transfer.y));
    vec3 x = clamp(c, 0.0, 1.0);
    return vec3(texture(srcLut, vec2(x.r, 0.5)).r,
                texture(srcLut, vec2(x.g, 0.5)).g,
                texture(srcLut, vec2(x.b, 0.5)).b);
}

vec3 encodeTransfer(vec3 c)
{
    float kind = ubuf.transfer.z;
    if (kind < 0.5)
        return c;
    if (kind < 1.5)
        return linearToSrgb(c);
    if (kind < 2.5)
        return pow(max(c, vec3(0.0)), vec3(1.0 / ubuf.transfer.w));
    vec3 x = sqrt(clamp(c, 0.0, 1.0));
    return vec3(texture(dstLut, vec2(x.r, 0.5)).r,
                texture(dstLut, vec2(x.g, 0.5)).g,
                texture(dstLut, vec2(x.b, 0.5)).b);
}

void main()
{
    // Derivatives in uniform control flow.
    vec2 dx = dFdx(v_uv);
    vec2 dy = dFdy(v_uv);
    vec2 footprint = vec2(length(dx * ubuf.texInfo.xy), length(dy * ubuf.texInfo.xy));
    // (tolerance: exactly 1:1 must take the single tap path despite rounding)

    vec4 c;
    if (ubuf.texInfo.z > 0.5 && max(footprint.x, footprint.y) > 1.001) {
        // Minification: 4 taps spread over the screen pixel's footprint, each from
        // one mip level finer than plain trilinear would use. Approximates a box
        // filter of the source pixels: sharper than trilinear, still alias free.
        c = texture(tex, v_uv + 0.25 * (-dx - dy), -1.0)
          + texture(tex, v_uv + 0.25 * ( dx - dy), -1.0)
          + texture(tex, v_uv + 0.25 * (-dx + dy), -1.0)
          + texture(tex, v_uv + 0.25 * ( dx + dy), -1.0);
        c *= 0.25;
    } else {
        c = texture(tex, v_uv);
    }

    float a = clamp(c.a, 0.0, 1.0);
    vec3 rgb = c.rgb;
    if (ubuf.flags.x > 0.5)
        rgb = a > 0.0 ? rgb / a : vec3(0.0);

    if (ubuf.flags.y < 0.5) {
        rgb = decodeTransfer(rgb);
        rgb = (ubuf.colorMatrix * vec4(rgb, 0.0)).rgb;
        rgb *= ubuf.range.x;
        rgb = clamp(rgb, 0.0, ubuf.range.y);
        rgb = encodeTransfer(rgb);
    }

    vec3 bg = ubuf.bgColor.rgb;
    if (ubuf.flags.z > 0.5) {
        vec2 cell = floor(gl_FragCoord.xy / ubuf.flags.w);
        if (mod(cell.x + cell.y, 2.0) >= 1.0)
            bg = ubuf.checkerColor.rgb;
    }
    fragColor = vec4(mix(bg, rgb, a), 1.0);
}
