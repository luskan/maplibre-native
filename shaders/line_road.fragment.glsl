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
uniform highp sampler2D u_road_guard;
in highp vec2 v_road_tile_pos;
in highp float v_road_ratio;
#pragma mapbox: define highp vec4 road_casing_color
#pragma mapbox: define highp float road_fill_width
#pragma mapbox: define highp float road_length
in highp float v_linesofar;
in vec2 v_width2;
in vec2 v_normal;
in float v_gamma_scale;

#pragma mapbox: define highp vec4 color
#pragma mapbox: define lowp float blur
#pragma mapbox: define lowp float opacity

void main() {
    #pragma mapbox: initialize highp vec4 road_casing_color
    #pragma mapbox: initialize highp float road_fill_width
    #pragma mapbox: initialize highp float road_length
    #pragma mapbox: initialize highp vec4 color
    #pragma mapbox: initialize lowp float blur
    #pragma mapbox: initialize lowp float opacity


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
