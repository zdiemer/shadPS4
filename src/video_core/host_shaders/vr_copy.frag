// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D source_image;
layout(push_constant) uniform settings { vec4 bounds; } params;
void main() {
    vec2 source_uv = mix(params.bounds.xy, params.bounds.zw, uv);
    color = any(lessThan(source_uv, vec2(0))) || any(greaterThan(source_uv, vec2(1)))
        ? vec4(0, 0, 0, 1) : vec4(texture(source_image, source_uv).rgb, 1);
}
