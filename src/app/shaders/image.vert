// SPDX-License-Identifier: GPL-3.0-or-later
#version 440

// One image tile: a unit quad mapped by `mvp` onto the tile's rectangle in the
// viewport (zoom, pan, rotation and flips are all part of mvp).

layout(location = 0) in vec2 position;
layout(location = 0) out vec2 v_uv;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    mat4 colorMatrix;
    vec4 uvRect;
    vec4 texInfo;
    vec4 transfer;
    vec4 flags;
    vec4 bgColor;
    vec4 checkerColor;
    vec4 range;
    vec4 tone;
} ubuf;

void main()
{
    v_uv = ubuf.uvRect.xy + position * ubuf.uvRect.zw;
    gl_Position = ubuf.mvp * vec4(position, 0.0, 1.0);
}
