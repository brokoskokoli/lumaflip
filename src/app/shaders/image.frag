// SPDX-License-Identifier: GPL-3.0-or-later
#version 440

// Image tile: high quality sampling (F-VIEW-06), color conversion into the output
// color space (F-COL-01, see ColorPipeline.h), exposure and HDR tone mapping (F-COL-03/05,
// core/HdrMapping.h - toneMapRgb() there is the reference of the code below) and
// compositing over the background or the transparency checkerboard (F-VIEW-09). Output is
// opaque. Overlay mode (texInfo.w): the tile is a UI overlay drawn over the image with its
// own alpha (extended range presentation, EdrSurface.h).

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    mat4 colorMatrix;   // upper 3x3: linear source -> linear output
    vec4 uvRect;
    vec4 texInfo;       // xy: texture size in texels, z: 1 = supersampled minification, w: 1 = overlay
    vec4 transfer;      // x: source kind, y: source gamma, z: output kind, w: output gamma
    vec4 flags;         // x: premultiplied, y: identity color, z: checker, w: checker cell (px)
    vec4 bgColor;       // background (and first checker color), output encoded
    vec4 checkerColor;  // second checker color
    vec4 range;         // x: exposure multiplier (linear), y: max linear value (1 = SDR), z: opacity,
                        // w: 1 = tone curve active
    vec4 tone;          // BT.2390 curve (HdrMapping.h ToneCurve): x knee, y srcPq, z kneePq, w maxLumPq
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

// SMPTE ST 2084 (PQ), as hdr::pqEncode/pqDecode.
const float PQ_M1 = 2610.0 / 16384.0;
const float PQ_M2 = 2523.0 / 4096.0 * 128.0;
const float PQ_C1 = 3424.0 / 4096.0;
const float PQ_C2 = 2413.0 / 4096.0 * 32.0;
const float PQ_C3 = 2392.0 / 4096.0 * 32.0;
const float REFERENCE_WHITE = 203.0;

float pqEncode(float nits)
{
    float p = pow(clamp(nits / 10000.0, 0.0, 1.0), PQ_M1);
    return pow((PQ_C1 + PQ_C2 * p) / (1.0 + PQ_C3 * p), PQ_M2);
}

float pqDecode(float signal)
{
    float e = pow(clamp(signal, 0.0, 1.0), 1.0 / PQ_M2);
    return 10000.0 * pow(max(e - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * e), 1.0 / PQ_M1);
}

// hdr::toneMapRgb: BT.2390 EETF on the largest component; identity below the knee.
vec3 toneMapRgb(vec3 rgb)
{
    float m = max(rgb.r, max(rgb.g, rgb.b));
    if (ubuf.range.w < 0.5 || m <= ubuf.tone.x)
        return rgb;
    float target = ubuf.range.y;
    float e1 = pqEncode(m * REFERENCE_WHITE) / ubuf.tone.y;
    float mapped = target;
    if (e1 < 1.0) {
        float ks = ubuf.tone.z;
        float t = (e1 - ks) / (1.0 - ks);
        float t2 = t * t;
        float t3 = t2 * t;
        float p = (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks)
                + (-2.0 * t3 + 3.0 * t2) * ubuf.tone.w;
        mapped = clamp(pqDecode(p * ubuf.tone.y) / REFERENCE_WHITE, ubuf.tone.x, target);
    }
    return rgb * (mapped / m);
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
        rgb = toneMapRgb(rgb);
        rgb = clamp(rgb, 0.0, ubuf.range.y);
        rgb = encodeTransfer(rgb);
    }

    if (ubuf.texInfo.w > 0.5) {
        // UI overlay: straight alpha, blended over the image (SrcAlpha, OneMinusSrcAlpha).
        fragColor = vec4(rgb, a);
        return;
    }

    vec3 bg = ubuf.bgColor.rgb;
    if (ubuf.flags.z > 0.5) {
        vec2 cell = floor(gl_FragCoord.xy / ubuf.flags.w);
        if (mod(cell.x + cell.y, 2.0) >= 1.0)
            bg = ubuf.checkerColor.rgb;
    }
    // Alpha = opacity of this image over the previous one (crossfade, F-SLIDE-02).
    fragColor = vec4(mix(bg, rgb, a), ubuf.range.z);
}
