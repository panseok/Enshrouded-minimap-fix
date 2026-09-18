"""Builds assets/embervale_minimap_map.frag.spv.

Equivalent GLSL (kept here as the source of truth):

    #version 450
    layout(location = 0) in vec2 inUv;            // 0..1 across the map quad
    layout(location = 0) out vec4 outColor;
    layout(set = 0, binding = 0) uniform sampler2D mapTexture;
    layout(push_constant) uniform MapPush {
        layout(offset = 32) vec4 view;    // xy = map uv under the player, z = uv per radius, w = radius px
        layout(offset = 48) vec4 params;  // x = cos(heading), y = sin(heading), z = vignette, w = shroud strength
    } pc;

    void main() {
        vec2 p = inUv * 2.0 - 1.0;                        // -1..1, +y = screen down
        float r = max(abs(p.x), abs(p.y));                  // square window
        float alpha = clamp((1.0 - r) * pc.view.w + 0.5, 0.0, 1.0);   // 1px anti-aliased edge
        vec2 n = vec2(pc.params.x * p.x - pc.params.y * p.y,
                      pc.params.y * p.x + pc.params.x * p.y);          // north-up offset
        vec2 uv = pc.view.xy + n * pc.view.z;
        vec4 texel = texture(mapTexture, uv);
        vec3 map = texel.rgb;
        // alpha = shroud signed distance: 128 at the border, >128 outside, 48 world units per side
        float d = (texel.a - 0.50196) * 96.378;
        float wpp = pc.view.z / pc.view.w * 10240.0;          // world units per pixel
        float shroud = (1.0 - smoothstep(-0.7 * wpp, 0.7 * wpp, d)) * pc.params.w;
        float edge = (1.0 - smoothstep(0.6 * wpp, 1.6 * wpp, abs(d))) * pc.params.w * 0.85;
        map = mix(map, map * 0.60 + vec3(0.17, 0.27, 0.36), shroud);   // light blue
        map = mix(map, vec3(0.62, 0.82, 0.96), edge);
        vec2 inside2 = step(vec2(0.0), uv) * step(uv, vec2(1.0));
        float inside = inside2.x * inside2.y;
        float vig = 1.0 - pc.params.z * r * r;
        map = clamp(map * vig, 0.0, 1.0);
        vec3 col = mix(vec3(0.045, 0.048, 0.042), map, inside);
        outColor = vec4(col, alpha);
    }

The rotation matches the CPU fallback in DrawRealMap exactly:
    northUpX = cos*sx - sin*sy ; northUpY = sin*sx + cos*sy
    u = (centerX + northUpX*upp) / W ; v = 1 - (centerZ - northUpY*upp) / W
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from spvasm import Module  # noqa: E402


def build():
    m = Module()
    f = m.t_float()
    v2, v3, v4 = m.t_vec(2), m.t_vec(3), m.t_vec(4)

    in_uv = m.variable("Input", v2, "inUv")
    m.decorate(in_uv, "Location", 0)
    out_color = m.variable("Output", v4, "outColor")
    m.decorate(out_color, "Location", 0)

    sampler = m.variable("UniformConstant", m.t_sampled_image2d(), "mapTexture")
    m.decorate(sampler, "DescriptorSet", 0)
    m.decorate(sampler, "Binding", 0)

    push_t = m.t_struct("MapPush", [v4, v4])
    m.name(push_t, "MapPush")
    m.decorate(push_t, "Block")
    m.member_decorate(push_t, 0, "Offset", 32)
    m.member_decorate(push_t, 1, "Offset", 48)
    pc = m.variable("PushConstant", push_t, "pc")
    ptr_pc_v4 = m.t_ptr("PushConstant", v4)

    c0, c1, c2, chalf = m.c_float(0.0), m.c_float(1.0), m.c_float(2.0), m.c_float(0.5)
    k0, k1 = m.c_int(0), m.c_int(1)
    vzero2 = m.c_vec(0.0, 0.0)
    vone2 = m.c_vec(1.0, 1.0)
    vone3 = m.c_vec(1.0, 1.0, 1.0)
    vzero3 = m.c_vec(0.0, 0.0, 0.0)
    bg = m.c_vec(0.045, 0.048, 0.042)

    m.begin_main("Fragment", [in_uv, out_color], origin_upper_left=True)

    uv0 = m.op("Load", v2, in_uv)
    p2 = m.op("VectorTimesScalar", v2, uv0, c2)
    p = m.op("FSub", v2, p2, vone2)
    # square window: Chebyshev distance
    p_abs = m.ext("FAbs", v2, p)
    p_ax = m.op("CompositeExtract", f, p_abs, 0)
    p_ay = m.op("CompositeExtract", f, p_abs, 1)
    r = m.ext("FMax", f, p_ax, p_ay)

    view_ptr = m.op("AccessChain", ptr_pc_v4, pc, k0)
    view = m.op("Load", v4, view_ptr)
    params_ptr = m.op("AccessChain", ptr_pc_v4, pc, k1)
    params = m.op("Load", v4, params_ptr)

    view_xy = m.op("VectorShuffle", v2, view, view, 0, 1)
    view_z = m.op("CompositeExtract", f, view, 2)
    view_w = m.op("CompositeExtract", f, view, 3)
    cos_h = m.op("CompositeExtract", f, params, 0)
    sin_h = m.op("CompositeExtract", f, params, 1)
    vig_k = m.op("CompositeExtract", f, params, 2)

    # rim alpha
    one_minus_r = m.op("FSub", f, c1, r)
    rim = m.op("FMul", f, one_minus_r, view_w)
    rim_b = m.op("FAdd", f, rim, chalf)
    alpha = m.ext("FClamp", f, rim_b, c0, c1)

    # rotate into north-up
    px = m.op("CompositeExtract", f, p, 0)
    py = m.op("CompositeExtract", f, p, 1)
    cx = m.op("FMul", f, cos_h, px)
    sy = m.op("FMul", f, sin_h, py)
    nx = m.op("FSub", f, cx, sy)
    sx = m.op("FMul", f, sin_h, px)
    cy = m.op("FMul", f, cos_h, py)
    ny = m.op("FAdd", f, sx, cy)
    n = m.op("CompositeConstruct", v2, nx, ny)
    n_scaled = m.op("VectorTimesScalar", v2, n, view_z)
    uv = m.op("FAdd", v2, view_xy, n_scaled)

    # sample (unconditionally, so implicit-LOD derivatives stay well defined)
    tex = m.op("Load", m.t_sampled_image2d(), sampler)
    sample = m.op("ImageSampleImplicitLod", v4, tex, uv)
    rgb0 = m.op("VectorShuffle", v3, sample, sample, 0, 1, 2)

    # shroud overlay from the alpha channel (signed distance, 128 = border)
    tex_a = m.op("CompositeExtract", f, sample, 3)
    a_off = m.op("FSub", f, tex_a, m.c_float(0.50196))
    d = m.op("FMul", f, a_off, m.c_float(96.378))
    upp = m.op("FDiv", f, view_z, view_w)
    wpp = m.op("FMul", f, upp, m.c_float(10240.0))
    strength = m.op("CompositeExtract", f, params, 3)
    e_in = m.op("FMul", f, wpp, m.c_float(0.7))
    e_in_neg = m.op("FNegate", f, e_in)
    ss_in = m.ext("SmoothStep", f, e_in_neg, e_in, d)
    shroud0 = m.op("FSub", f, c1, ss_in)
    shroud = m.op("FMul", f, shroud0, strength)
    abs_d = m.ext("FAbs", f, d)
    e0 = m.op("FMul", f, wpp, m.c_float(0.6))
    e1 = m.op("FMul", f, wpp, m.c_float(1.6))
    ss_edge = m.ext("SmoothStep", f, e0, e1, abs_d)
    edge0 = m.op("FSub", f, c1, ss_edge)
    edge1 = m.op("FMul", f, edge0, strength)
    edge = m.op("FMul", f, edge1, m.c_float(0.85))
    dark = m.op("VectorTimesScalar", v3, rgb0, m.c_float(0.60))
    tinted = m.op("FAdd", v3, dark, m.c_vec(0.17, 0.27, 0.36))
    shroud3 = m.op("CompositeConstruct", v3, shroud, shroud, shroud)
    rgb1 = m.ext("FMix", v3, rgb0, tinted, shroud3)
    edge3 = m.op("CompositeConstruct", v3, edge, edge, edge)
    rgb = m.ext("FMix", v3, rgb1, m.c_vec(0.62, 0.82, 0.96), edge3)

    # inside-world mask
    s_lo = m.ext("Step", v2, vzero2, uv)
    s_hi = m.ext("Step", v2, uv, vone2)
    s_both = m.op("FMul", v2, s_lo, s_hi)
    in_x = m.op("CompositeExtract", f, s_both, 0)
    in_y = m.op("CompositeExtract", f, s_both, 1)
    inside = m.op("FMul", f, in_x, in_y)

    # vignette
    rr = m.op("FMul", f, r, r)
    vr = m.op("FMul", f, vig_k, rr)
    vig = m.op("FSub", f, c1, vr)
    shaded = m.op("VectorTimesScalar", v3, rgb, vig)
    shaded_c = m.ext("FClamp", v3, shaded, vzero3, vone3)

    inside3 = m.op("CompositeConstruct", v3, inside, inside, inside)
    col = m.ext("FMix", v3, bg, shaded_c, inside3)

    cr = m.op("CompositeExtract", f, col, 0)
    cg = m.op("CompositeExtract", f, col, 1)
    cb = m.op("CompositeExtract", f, col, 2)
    out = m.op("CompositeConstruct", v4, cr, cg, cb, alpha)
    m.store(out_color, out)
    m.end_main()
    return m.assemble()


if __name__ == "__main__":
    dest = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "assets", "embervale_minimap_map.frag.spv")
    data = build()
    with open(dest, "wb") as fh:
        fh.write(data)
    print(f"wrote {dest} ({len(data)} bytes)")
