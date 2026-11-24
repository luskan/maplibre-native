// floor(127 / 2) == 63.0
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

#pragma mapbox: define highp vec4 color
#pragma mapbox: define lowp float blur
#pragma mapbox: define lowp float opacity
#pragma mapbox: define mediump float gapwidth
#pragma mapbox: define lowp float offset
#pragma mapbox: define mediump float width
#pragma mapbox: define highp vec4 traffic_left_color

#pragma mapbox: define highp float traffic_marker
#pragma mapbox: define highp float traffic_origin
flat out highp float v_marker_column;
flat out highp vec4 v_traffic_outline;
flat out highp vec4 v_traffic_edges;
out highp float v_marker_phase;

void main() {
    #pragma mapbox: initialize highp float traffic_marker
    #pragma mapbox: initialize highp float traffic_origin
    #pragma mapbox: initialize highp vec4 color
    #pragma mapbox: initialize lowp float blur
    #pragma mapbox: initialize lowp float opacity
    #pragma mapbox: initialize mediump float gapwidth
    #pragma mapbox: initialize lowp float offset
    #pragma mapbox: initialize mediump float width
    #pragma mapbox: initialize highp vec4 traffic_left_color

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
