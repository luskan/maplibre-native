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
uniform highp sampler2D u_road_guard;
in highp vec2 v_road_tile_pos;
in highp float v_road_ratio;
#pragma mapbox: define highp vec4 road_casing_color
#pragma mapbox: define highp float road_fill_width
#pragma mapbox: define highp float road_length
#pragma mapbox: define highp vec4 traffic_left_color
flat in highp vec3 v_road_profile_interval;
in highp float v_linesofar;
in vec2 v_width2;
in vec2 v_normal;
in float v_gamma_scale;

#pragma mapbox: define highp vec4 color
#pragma mapbox: define lowp float blur
#pragma mapbox: define lowp float opacity

#if !defined(HAS_UNIFORM_u_path_distance) && !defined(HAS_UNIFORM_u_no_traffic_atlas)
uniform highp sampler2D u_traffic_atlas;
#endif
flat in highp float v_marker_column;
flat in highp vec4 v_traffic_outline;
flat in highp vec4 v_traffic_edges;
in highp float v_marker_phase;

void main() {
    #pragma mapbox: initialize highp vec4 road_casing_color
    #pragma mapbox: initialize highp float road_fill_width
    #pragma mapbox: initialize highp float road_length
    #pragma mapbox: initialize highp vec4 traffic_left_color
    #pragma mapbox: initialize highp vec4 color
    #pragma mapbox: initialize lowp float blur
    #pragma mapbox: initialize lowp float opacity


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
