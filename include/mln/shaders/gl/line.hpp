// Generated code, do not modify this file!
#pragma once
#include <mln/shaders/shader_source.hpp>

namespace mln {
namespace shaders {

template <>
struct ShaderSource<BuiltIn::LineShader, gfx::Backend::Type::OpenGL> {
    static constexpr const char* name = "LineShader";
    static constexpr const char* vertex = R"(// floor(127 / 2) == 63.0
// the maximum allowed miter limit is 2.0 at the moment. the extrude normal is
// stored in a byte (-128..127). we scale regular normals up to length 63, but
// there are also "special" normals that have a bigger length (of up to 126 in
// this case).
// #define scale 63.0
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
    // Interpolations
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

out vec2 v_normal;
out vec2 v_width2;
out float v_gamma_scale;
out highp float v_linesofar;

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
#ifndef HAS_UNIFORM_u_traffic_left_color
layout (location = 9) in highp vec4 a_traffic_left_color;
out highp vec4 traffic_left_color;
#endif

#ifndef HAS_UNIFORM_u_traffic_marker
layout (location = 10) in highp vec2 a_traffic_marker;
#endif
#ifndef HAS_UNIFORM_u_traffic_origin
layout (location = 11) in highp vec2 a_traffic_origin;
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
    #ifndef HAS_UNIFORM_u_traffic_left_color
traffic_left_color = unpack_mix_color(a_traffic_left_color, u_traffic_left_color_t);
#else
highp vec4 traffic_left_color = u_traffic_left_color;
#endif

    // the distance over which the line edge fades out.
    // Retina devices need a smaller distance to avoid aliasing.
    float ANTIALIASING = 1.0 / DEVICE_PIXEL_RATIO / 2.0;

    vec2 a_extrude = a_data.xy - 128.0;
    float a_direction = mod(a_data.z, 4.0) - 1.0;

    v_linesofar = (floor(a_data.z / 4.0) + a_data.w * 64.0) * 2.0;

    vec2 pos = floor(a_pos_normal * 0.5);

    // x is 1 if it's a round cap, 0 otherwise
    // y is 1 if the normal points up, and -1 if it points down
    // We store these in the least significant bit of a_pos_normal
    mediump vec2 normal = a_pos_normal - 2.0 * pos;
    normal.y = normal.y * 2.0 - 1.0;
    v_normal = normal;

    // these transformations used to be applied in the JS and native code bases.
    // moved them into the shader for clarity and simplicity.
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

    // Scale the extrusion vector down to a normal and then up by the line width
    // of this vertex.
    mediump vec2 dist = outset * a_extrude * scale;

    // Calculate the offset when drawing a line that is to the side of the actual line.
    // We do this by creating a vector that points towards the extrude, but rotate
    // it when we're drawing round end points (a_direction = -1 or 1) since their
    // extrude vector points in another direction.
    mediump float u = 0.5 * a_direction;
    mediump float t = 1.0 - abs(u);
    mediump vec2 offset2 = offset * a_extrude * scale * normal.y * mat2(t, -u, u, t);

    vec4 projected_extrude = u_matrix * vec4(dist / u_ratio, 0.0, 0.0);
    gl_Position = u_matrix * vec4(pos + offset2 / u_ratio, 0.0, 1.0) + projected_extrude;

    // calculate how much the perspective view squishes or stretches the extrude
    float extrude_length_without_perspective = length(dist);
    float extrude_length_with_perspective = length(projected_extrude.xy / gl_Position.w * u_units_to_pixels);
    v_gamma_scale = extrude_length_without_perspective / extrude_length_with_perspective;

#if !defined(HAS_UNIFORM_u_path_distance) && !defined(HAS_UNIFORM_u_no_traffic_atlas)
    v_marker_column = u_traffic_atlas_aspect > 0.0 && traffic_marker > 0.0 ? float(slot + 1) : 0.0;
    highp float width_for_marker = max(width, 0.01);
    highp float period = max(width_for_marker * u_traffic_atlas_aspect, 0.01);
    v_marker_phase = (traffic_origin + pathDistance) * u_ratio / period;
#else
    v_marker_column = 0.0;
    v_marker_phase = 0.0;
#endif
    v_width2 = vec2(outset, inset);
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

in vec2 v_width2;
in vec2 v_normal;
in float v_gamma_scale;

#ifndef HAS_UNIFORM_u_color
in highp vec4 color;
#endif
#ifndef HAS_UNIFORM_u_traffic_left_color
in highp vec4 traffic_left_color;
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
    #ifdef HAS_UNIFORM_u_color
highp vec4 color = u_color;
#endif
    #ifdef HAS_UNIFORM_u_traffic_left_color
highp vec4 traffic_left_color = u_traffic_left_color;
#endif
    #ifdef HAS_UNIFORM_u_blur
lowp float blur = u_blur;
#endif
    #ifdef HAS_UNIFORM_u_opacity
lowp float opacity = u_opacity;
#endif

    // Calculate the distance of the pixel from the line in pixels.
    if (v_normal.x < 0.0) discard;
    float dist = length(v_normal) * v_width2.s;

    // Calculate the antialiasing fade factor. This is either when fading in
    // the line in case of an offset line (v_width2.t) or when fading out
    // (v_width2.s)
    float blur2 = (blur + 1.0 / DEVICE_PIXEL_RATIO) * v_gamma_scale;
    float alpha = clamp(min(dist - (v_width2.t - blur2), v_width2.s - dist) / blur2, 0.0, 1.0);

    vec4 side_color = (v_normal.y > 0.0 && traffic_left_color.a > 0.0) ? traffic_left_color : color;
#ifndef HAS_UNIFORM_u_path_distance
    if (v_traffic_edges.x > 0.5) {
        if (v_traffic_edges.z == 0.0 && traffic_left_color.a > 0.0)
            side_color = mix(color, traffic_left_color, clamp(0.5 + v_normal.y * 0.5 * v_traffic_edges.w, 0.0, 1.0));
        highp float transverse = abs(v_normal.y);
        // Blend adjacent base texture texels at their original positions across the road.
        highp float paint = v_traffic_edges.y > 0.0
            ? clamp(0.5 + ((1.0 - transverse) * 0.5 - v_traffic_edges.y) * v_traffic_edges.w, 0.0, 1.0) : 1.0;
        if (v_traffic_edges.z > 0.0)
            paint = min(paint, clamp(0.5 + (transverse * 0.5 - v_traffic_edges.z) * v_traffic_edges.w, 0.0, 1.0));
        side_color = mix(v_traffic_outline, side_color, paint);
    }
#endif
#if !defined(HAS_UNIFORM_u_path_distance) && !defined(HAS_UNIFORM_u_no_traffic_atlas)
    if (v_marker_column >= 1.0 && v_marker_column <= 8.0) {
        highp float across = clamp(v_normal.y * 0.5 + 0.5, 0.001, 0.999);
        highp float phase = v_marker_phase;
        vec4 mark = texture(u_traffic_atlas, vec2((v_marker_column - 1.0 + across) / 8.0, phase));
        side_color = mark + side_color * (1.0 - mark.a);
    }
#endif
    fragColor = side_color * (alpha * opacity);

#ifdef OVERDRAW_INSPECTOR
    fragColor = vec4(1.0);
#endif
}
)";
};

} // namespace shaders
} // namespace mln
