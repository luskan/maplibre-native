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
#ifndef HAS_UNIFORM_u_path_distance
layout (location = 2) in highp float a_path_distance;
#else
const highp float a_path_distance = 0.0;
#endif

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

#ifndef HAS_UNIFORM_u_path_distance
layout (std140) uniform LineTrafficPaintUBO {
    highp vec4 u_traffic_widths[5];
    highp vec4 u_traffic_scales;
    highp vec4 u_traffic_outline;
    highp vec4 u_traffic_insets;
};
#endif

layout (std140) uniform LineDrawableUBO {
    highp mat4 u_matrix;
    mediump float u_ratio;

    lowp float u_color_t;
    lowp float u_blur_t;
    lowp float u_opacity_t;
    lowp float u_gapwidth_t;
    lowp float u_offset_t;
    lowp float u_width_t;
    lowp float u_traffic_left_color_t;
    lowp float u_traffic_marker_t;
    lowp float u_traffic_origin_t;
    highp float u_traffic_atlas_aspect;
    lowp float traffic_pad;
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
    highp vec4 u_traffic_left_color;
    highp float u_traffic_marker;
    highp float u_traffic_origin;
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
    highp float u_road_profile_range;
};
out highp vec2 v_road_tile_pos;
out highp float v_road_ratio;
out vec2 v_normal;
out vec2 v_width2;
out float v_gamma_scale;
out highp float v_linesofar;
flat out highp vec3 v_road_profile_interval;

#ifndef HAS_UNIFORM_u_color
layout (location = 3) in highp vec4 a_color;
out highp vec4 color;
#endif
#ifndef HAS_UNIFORM_u_blur
layout (location = 4) in lowp vec2 a_blur;
out lowp float blur;
#endif
#ifndef HAS_UNIFORM_u_opacity
layout (location = 5) in lowp vec2 a_opacity;
out lowp float opacity;
#endif
#ifndef HAS_UNIFORM_u_gapwidth
layout (location = 6) in mediump vec2 a_gapwidth;
#endif
#ifndef HAS_UNIFORM_u_offset
layout (location = 7) in lowp vec2 a_offset;
#endif
#ifndef HAS_UNIFORM_u_width
layout (location = 8) in mediump vec2 a_width;
#endif
#ifndef HAS_UNIFORM_u_road_casing_color
layout (location = 9) in highp vec4 a_road_casing_color;
out highp vec4 road_casing_color;
#endif
#ifndef HAS_UNIFORM_u_road_fill_width
layout (location = 10) in highp vec2 a_road_fill_width;
out highp float road_fill_width;
#endif
#ifndef HAS_UNIFORM_u_road_length
layout (location = 11) in highp vec2 a_road_length;
out highp float road_length;
#endif
#ifndef HAS_UNIFORM_u_traffic_left_color
layout (location = 12) in highp vec4 a_traffic_left_color;
out highp vec4 traffic_left_color;
#endif
#ifndef HAS_UNIFORM_u_road_profile_range
layout (location = 13) in highp vec2 a_road_profile_range;
#endif

// Packed ranges are copied verbatim from the feature at both zoom stops.
#define u_road_profile_range_t 0.0

#ifndef HAS_UNIFORM_u_traffic_marker
layout (location = 14) in highp vec2 a_traffic_marker;
#endif
#ifndef HAS_UNIFORM_u_traffic_origin
layout (location = 15) in highp vec2 a_traffic_origin;
#endif
flat out highp float v_marker_column;
flat out highp vec4 v_traffic_outline;
flat out highp vec4 v_traffic_edges;
out highp float v_marker_phase;

void main() {
    #ifndef HAS_UNIFORM_u_traffic_marker
highp float traffic_marker = unpack_mix_vec2(a_traffic_marker, u_traffic_marker_t);
#else
highp float traffic_marker = u_traffic_marker;
#endif
    #ifndef HAS_UNIFORM_u_traffic_origin
highp float traffic_origin = unpack_mix_vec2(a_traffic_origin, u_traffic_origin_t);
#else
highp float traffic_origin = u_traffic_origin;
#endif
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
    #ifndef HAS_UNIFORM_u_traffic_left_color
traffic_left_color = unpack_mix_color(a_traffic_left_color, u_traffic_left_color_t);
#else
highp vec4 traffic_left_color = u_traffic_left_color;
#endif
    #ifndef HAS_UNIFORM_u_road_profile_range
highp float road_profile_range = unpack_mix_vec2(a_road_profile_range, u_road_profile_range_t);
#else
highp float road_profile_range = u_road_profile_range;
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

    bool reverse_side = road_profile_range < 0.0;
    highp float packed_range = reverse_side ? -1.0 - road_profile_range : road_profile_range;
    v_road_profile_interval = vec3(floor(packed_range / 2048.0),
        mod(packed_range, 2048.0), reverse_side ? -1.0 : 1.0);



    float ANTIALIASING = 1.0 / DEVICE_PIXEL_RATIO / 2.0;

    vec2 a_extrude = a_data.xy - 128.0;
    float a_direction = mod(a_data.z, 4.0) - 1.0;

    v_linesofar = (floor(a_data.z / 4.0) + a_data.w * 64.0) * 2.0;

    vec2 pos = floor(a_pos_normal * 0.5);




    mediump vec2 normal = a_pos_normal - 2.0 * pos;
    normal.y = normal.y * 2.0 - 1.0;
    v_normal = normal;



    gapwidth = gapwidth / 2.0;
#ifndef HAS_UNIFORM_u_path_distance
    int code = int(abs(traffic_marker) + 0.5);
    int slot = max(code - 1, 0) % 8;
    int category = clamp((max(code - 1, 0) / 16), 0, 19);
    bool speedProfile = code > 0 && u_traffic_scales.z > 0.0;
    highp float pathDistance = a_path_distance < 0.0 ? -1.0 - a_path_distance : a_path_distance;
    if (speedProfile && a_path_distance < 0.0) v_normal.x = -normal.x;
    if (speedProfile) {
        width = traffic_marker > 0.0 ? u_traffic_widths[category / 4][category % 4]
            * (slot >= 4 ? u_traffic_scales.y : u_traffic_scales.x) : 0.0;
    }
    bool split = ((max(code - 1, 0) / 8) % 2) != 0;
    v_traffic_edges = vec4(speedProfile ? 1.0 : 0.0,
        split ? u_traffic_insets.y : u_traffic_insets.x, split ? u_traffic_insets.z : -1.0, u_traffic_insets.w);
    v_traffic_outline = u_traffic_outline;
#else
    v_traffic_edges = vec4(0.0);
    v_traffic_outline = vec4(0.0);
#endif
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

#if !defined(HAS_UNIFORM_u_path_distance) && !defined(HAS_UNIFORM_u_no_traffic_atlas)
    v_marker_column = u_traffic_atlas_aspect > 0.0 && traffic_marker > 0.0 ? float(slot + 1) : 0.0;
    highp float width_for_marker = max(speedProfile ? width : road_fill_width, 0.01);
    highp float period = max(width_for_marker * u_traffic_atlas_aspect, 0.01);
    v_marker_phase = (traffic_origin + pathDistance * v_road_profile_interval.z) * u_ratio / period;
#else
    v_marker_column = 0.0;
    v_marker_phase = 0.0;
#endif
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
    highp vec4 u_traffic_left_color;
    highp float u_traffic_marker;
    highp float u_traffic_origin;
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
    highp float u_road_profile_range;
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
#ifndef HAS_UNIFORM_u_traffic_left_color
in highp vec4 traffic_left_color;
#endif
flat in highp vec3 v_road_profile_interval;
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

#if !defined(HAS_UNIFORM_u_path_distance) && !defined(HAS_UNIFORM_u_no_traffic_atlas)
uniform highp sampler2D u_traffic_atlas;
#endif
flat in highp float v_marker_column;
flat in highp vec4 v_traffic_outline;
flat in highp vec4 v_traffic_edges;
in highp float v_marker_phase;

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
    #ifdef HAS_UNIFORM_u_traffic_left_color
highp vec4 traffic_left_color = u_traffic_left_color;
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


    if (v_normal.x < 0.0) discard;
    float dist = length(v_normal) * v_width2.s;




    float blur2 = (blur + 1.0 / DEVICE_PIXEL_RATIO) * v_gamma_scale;
    float alpha = clamp(min(dist - (v_width2.t - blur2), v_width2.s - dist) / blur2, 0.0, 1.0);

    float side = v_normal.y * v_road_profile_interval.z;
    vec4 fill_color = (side > 0.0 && traffic_left_color.a > 0.0) ? traffic_left_color : color;
#ifndef HAS_UNIFORM_u_path_distance
    if (v_traffic_edges.x > 0.5) {
        if (v_traffic_edges.z == 0.0 && traffic_left_color.a > 0.0)
            fill_color = mix(color, traffic_left_color, clamp(0.5 + side * 0.5 * v_traffic_edges.w, 0.0, 1.0));
        highp float transverse = abs(v_normal.y);
        // Blend adjacent base texture texels at their original positions across the road.
        highp float paint = v_traffic_edges.y > 0.0
            ? clamp(0.5 + ((1.0 - transverse) * 0.5 - v_traffic_edges.y) * v_traffic_edges.w, 0.0, 1.0) : 1.0;
        if (v_traffic_edges.z > 0.0)
            paint = min(paint, clamp(0.5 + (transverse * 0.5 - v_traffic_edges.z) * v_traffic_edges.w, 0.0, 1.0));
        fill_color = mix(v_traffic_outline, fill_color, paint);
    }
#endif
#if !defined(HAS_UNIFORM_u_path_distance) && !defined(HAS_UNIFORM_u_no_traffic_atlas)
    if (v_marker_column >= 1.0 && v_marker_column <= 8.0) {
        highp float across = clamp(v_normal.y * 0.5 + 0.5, 0.001, 0.999);
        highp float phase = v_marker_phase;
        vec4 mark = texture(u_traffic_atlas, vec2((v_marker_column - 1.0 + across) / 8.0, phase));
        fill_color = mark + fill_color * (1.0 - mark.a);
    }
#endif
    if ((v_traffic_edges.x < 0.5 && fill_color.a < 0.99999) || road_casing_color.a < 0.99999 || opacity < 0.99999
        || u_gapwidth != 0.0 || u_offset != 0.0 || blur != 0.0) {
        discard;
    }
    highp float protection;
    if (u_road_guard_width == 0.0) {
        if (!(road_length > 0.0 && road_length < 16384.0)) discard;
        highp float first = v_road_profile_interval.x;
        highp float last = v_road_profile_interval.y;
        highp float position = v_linesofar / road_length * 2047.0;
        if (position < first || (last < 2047.0 && position >= last)) discard;
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
    vec4 profile = v_traffic_edges.x > 0.5 ? fill_color : mix(road_casing_color, fill_color, fill_weight);
    fragColor = profile * (alpha * protection);

#ifdef OVERDRAW_INSPECTOR
    fragColor = vec4(1.0);
#endif
}
)";
};

} // namespace shaders
} // namespace mln
