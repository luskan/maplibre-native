// Generated code, do not modify this file!
#pragma once
#include <mln/shaders/shader_source.hpp>

namespace mln {
namespace shaders {

template <>
struct ShaderSource<BuiltIn::LineRoadShader, gfx::Backend::Type::OpenGL> {
    static constexpr const char* name = "LineRoadShader";
    static constexpr const char* vertex = R"(





#define scale 0.015873016

layout (location = 0) in vec2 a_pos_normal;
layout (location = 1) in vec4 a_data;

layout (std140) uniform GlobalPaintParamsUBO {
    highp vec2 u_pattern_atlas_texsize;
    highp vec2 u_units_to_pixels;
    highp vec2 u_world_size;
    highp float u_camera_to_center_distance;
    highp float u_symbol_fade_change;
    highp float u_aspect_ratio;
    highp float u_pixel_ratio;
    highp float u_map_zoom;
    lowp float global_pad1;
};

layout (std140) uniform LineDrawableUBO {
    highp mat4 u_matrix;
    mediump float u_ratio;

    lowp float u_color_t;
    lowp float u_blur_t;
    lowp float u_opacity_t;
    lowp float u_gapwidth_t;
    lowp float u_offset_t;
    lowp float u_width_t;
    lowp float drawable_pad1;
};

layout (std140) uniform LineEvaluatedPropsUBO {
    highp vec4 u_color;
    lowp float u_blur;
    lowp float u_opacity;
    mediump float u_gapwidth;
    lowp float u_offset;
    mediump float u_width;
    lowp float u_floorwidth;
    lowp float props_pad1;
    lowp float props_pad2;
};

layout (std140) uniform LineRoadPropsUBO {
    highp vec4 u_road_casing_color;
    highp vec4 u_road_guard_rect;
    highp float u_road_fill_width;
    highp float u_road_guard_width;
    highp float u_road_fill_width_t;
    highp float u_road_casing_color_t;
    highp float u_road_max_guard_distance;
    highp float u_road_length;
    highp float u_road_length_t;
    highp float road_pad3;
};
out highp vec2 v_road_tile_pos;
out highp float v_road_ratio;
out vec2 v_normal;
out vec2 v_width2;
out float v_gamma_scale;
out highp float v_linesofar;

#ifndef HAS_UNIFORM_u_color
layout (location = 2) in highp vec4 a_color;
out highp vec4 color;
#endif
#ifndef HAS_UNIFORM_u_blur
layout (location = 3) in lowp vec2 a_blur;
out lowp float blur;
#endif
#ifndef HAS_UNIFORM_u_opacity
layout (location = 4) in lowp vec2 a_opacity;
out lowp float opacity;
#endif
#ifndef HAS_UNIFORM_u_gapwidth
layout (location = 5) in mediump vec2 a_gapwidth;
#endif
#ifndef HAS_UNIFORM_u_offset
layout (location = 6) in lowp vec2 a_offset;
#endif
#ifndef HAS_UNIFORM_u_width
layout (location = 7) in mediump vec2 a_width;
#endif
#ifndef HAS_UNIFORM_u_road_casing_color
layout (location = 8) in highp vec4 a_road_casing_color;
out highp vec4 road_casing_color;
#endif
#ifndef HAS_UNIFORM_u_road_fill_width
layout (location = 9) in highp vec2 a_road_fill_width;
out highp float road_fill_width;
#endif
#ifndef HAS_UNIFORM_u_road_length
layout (location = 10) in highp vec2 a_road_length;
out highp float road_length;
#endif

void main() {
    #ifndef HAS_UNIFORM_u_road_casing_color
road_casing_color = unpack_mix_color(a_road_casing_color, u_road_casing_color_t);
#else
highp vec4 road_casing_color = u_road_casing_color;
#endif
    #ifndef HAS_UNIFORM_u_road_fill_width
road_fill_width = unpack_mix_vec2(a_road_fill_width, u_road_fill_width_t);
#else
highp float road_fill_width = u_road_fill_width;
#endif
    #ifndef HAS_UNIFORM_u_road_length
road_length = unpack_mix_vec2(a_road_length, u_road_length_t);
#else
highp float road_length = u_road_length;
#endif
    #ifndef HAS_UNIFORM_u_color
color = unpack_mix_color(a_color, u_color_t);
#else
highp vec4 color = u_color;
#endif
    #ifndef HAS_UNIFORM_u_blur
blur = unpack_mix_vec2(a_blur, u_blur_t);
#else
lowp float blur = u_blur;
#endif
    #ifndef HAS_UNIFORM_u_opacity
opacity = unpack_mix_vec2(a_opacity, u_opacity_t);
#else
lowp float opacity = u_opacity;
#endif
    #ifndef HAS_UNIFORM_u_gapwidth
mediump float gapwidth = unpack_mix_vec2(a_gapwidth, u_gapwidth_t);
#else
mediump float gapwidth = u_gapwidth;
#endif
    #ifndef HAS_UNIFORM_u_offset
lowp float offset = unpack_mix_vec2(a_offset, u_offset_t);
#else
lowp float offset = u_offset;
#endif
    #ifndef HAS_UNIFORM_u_width
mediump float width = unpack_mix_vec2(a_width, u_width_t);
#else
mediump float width = u_width;
#endif



    float ANTIALIASING = 1.0 / DEVICE_PIXEL_RATIO / 2.0;

    vec2 a_extrude = a_data.xy - 128.0;
    float a_direction = mod(a_data.z, 4.0) - 1.0;

    v_linesofar = (floor(a_data.z / 4.0) + a_data.w * 64.0) * 2.0;

    vec2 pos = floor(a_pos_normal * 0.5);




    mediump vec2 normal = a_pos_normal - 2.0 * pos;
    normal.y = normal.y * 2.0 - 1.0;
    v_normal = normal;



    gapwidth = gapwidth / 2.0;
    float halfwidth = width / 2.0;
    offset = -1.0 * offset;

    float inset = gapwidth + (gapwidth > 0.0 ? ANTIALIASING : 0.0);
    float outset = gapwidth + halfwidth * (gapwidth > 0.0 ? 2.0 : 1.0) + (halfwidth == 0.0 ? 0.0 : ANTIALIASING);



    mediump vec2 dist = outset * a_extrude * scale;





    mediump float u = 0.5 * a_direction;
    mediump float t = 1.0 - abs(u);
    mediump vec2 offset2 = offset * a_extrude * scale * normal.y * mat2(t, -u, u, t);

    vec4 projected_extrude = u_matrix * vec4(dist / u_ratio, 0.0, 0.0);
    gl_Position = u_matrix * vec4(pos + offset2 / u_ratio, 0.0, 1.0) + projected_extrude;


    float extrude_length_without_perspective = length(dist);
    float extrude_length_with_perspective = length(projected_extrude.xy / gl_Position.w * u_units_to_pixels);
    v_gamma_scale = extrude_length_without_perspective / extrude_length_with_perspective;

    v_width2 = vec2(outset, inset);
    v_road_tile_pos = pos + (offset2 + dist) / u_ratio;
    v_road_ratio = u_ratio;
}
)";
    static constexpr const char* fragment = R"(layout (std140) uniform LineEvaluatedPropsUBO {
    highp vec4 u_color;
    lowp float u_blur;
    lowp float u_opacity;
    mediump float u_gapwidth;
    lowp float u_offset;
    mediump float u_width;
    lowp float u_floorwidth;
    lowp float props_pad1;
    lowp float props_pad2;
};

layout (std140) uniform LineRoadPropsUBO {
    highp vec4 u_road_casing_color;
    highp vec4 u_road_guard_rect;
    highp float u_road_fill_width;
    highp float u_road_guard_width;
    highp float u_road_fill_width_t;
    highp float u_road_casing_color_t;
    highp float u_road_max_guard_distance;
    highp float u_road_length;
    highp float u_road_length_t;
    highp float road_pad3;
};
uniform highp sampler2D u_road_guard;
in highp vec2 v_road_tile_pos;
in highp float v_road_ratio;
#ifndef HAS_UNIFORM_u_road_casing_color
in highp vec4 road_casing_color;
#endif
#ifndef HAS_UNIFORM_u_road_fill_width
in highp float road_fill_width;
#endif
#ifndef HAS_UNIFORM_u_road_length
in highp float road_length;
#endif
in highp float v_linesofar;
in vec2 v_width2;
in vec2 v_normal;
in float v_gamma_scale;

#ifndef HAS_UNIFORM_u_color
in highp vec4 color;
#endif
#ifndef HAS_UNIFORM_u_blur
in lowp float blur;
#endif
#ifndef HAS_UNIFORM_u_opacity
in lowp float opacity;
#endif

void main() {
    #ifdef HAS_UNIFORM_u_road_casing_color
highp vec4 road_casing_color = u_road_casing_color;
#endif
    #ifdef HAS_UNIFORM_u_road_fill_width
highp float road_fill_width = u_road_fill_width;
#endif
    #ifdef HAS_UNIFORM_u_road_length
highp float road_length = u_road_length;
#endif
    #ifdef HAS_UNIFORM_u_color
highp vec4 color = u_color;
#endif
    #ifdef HAS_UNIFORM_u_blur
lowp float blur = u_blur;
#endif
    #ifdef HAS_UNIFORM_u_opacity
lowp float opacity = u_opacity;
#endif


    float dist = length(v_normal) * v_width2.s;




    float blur2 = (blur + 1.0 / DEVICE_PIXEL_RATIO) * v_gamma_scale;
    float alpha = clamp(min(dist - (v_width2.t - blur2), v_width2.s - dist) / blur2, 0.0, 1.0);

    if (color.a < 0.99999 || road_casing_color.a < 0.99999 || opacity < 0.99999
        || u_gapwidth != 0.0 || u_offset != 0.0 || blur != 0.0) {
        discard;
    }
    highp float protection;
    if (u_road_guard_width == 0.0) {
        if (!(road_length > 0.0 && road_length < 16384.0)) discard;
        // Fade over four unpitched logical map pixels at both source ends.
        protection = clamp(min(v_linesofar, road_length - v_linesofar) * v_road_ratio / 4.0, 0.0, 1.0);
    } else {
        // The baseline supplies the outer AA edge for guarded whole-road composition.
        if (alpha < 0.99999) discard;
        highp vec2 uv = (v_road_tile_pos - u_road_guard_rect.xy) / u_road_guard_rect.zw;
        highp float radius = 1.125 * u_road_guard_width;
        highp float fade = 1.0 / DEVICE_PIXEL_RATIO;
        highp float edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
        highp float edge_distance = edge * min(u_road_guard_rect.z, u_road_guard_rect.w) * v_road_ratio;
        if (!(radius > 0.0) || edge_distance <= radius + fade
            || u_road_max_guard_distance * v_road_ratio <= radius + fade) discard;
        highp vec2 packed_distance = floor(texture(u_road_guard, uv).rg * 255.0 + 0.5);
        highp float guard_distance = dot(packed_distance, vec2(256.0, 1.0)) * v_road_ratio;
        protection = clamp((guard_distance - radius) / fade, 0.0, 1.0);
    }
    float inner_outset = min(road_fill_width * 0.5,
        max(0.0, v_width2.s - 0.5 / DEVICE_PIXEL_RATIO)) + 0.5 / DEVICE_PIXEL_RATIO;
    float fill_weight = clamp((inner_outset - dist) / blur2, 0.0, 1.0);
    vec4 profile = mix(road_casing_color, color, fill_weight);
    fragColor = profile * (alpha * protection);

#ifdef OVERDRAW_INSPECTOR
    fragColor = vec4(1.0);
#endif
}
)";
};

} // namespace shaders
} // namespace mln
