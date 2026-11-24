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

in vec2 v_width2;
in vec2 v_normal;
in float v_gamma_scale;

#pragma mapbox: define highp vec4 color
#pragma mapbox: define highp vec4 traffic_left_color
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
    #pragma mapbox: initialize highp vec4 color
    #pragma mapbox: initialize highp vec4 traffic_left_color
    #pragma mapbox: initialize lowp float blur
    #pragma mapbox: initialize lowp float opacity

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
