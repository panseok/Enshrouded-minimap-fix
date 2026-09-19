"""Builds assets/embervale_minimap_fog.frag.spv.

Fog-of-war overlay drawn over the map quad with the same push constants as the
map shader. The texture is the live FogOfWar grid copied from the game (one texel
per 8 world units, world 0..10240 mapped to uv 0..1 exactly like the map, red
channel = discovered 0..1).

Equivalent GLSL (kept here as the source of truth):

    #version 450
    layout(location = 0) in vec2 inUv;
    layout(location = 0) out vec4 outColor;
    layout(set = 0, binding = 0) uniform sampler2D fogTexture;
    layout(push_constant) uniform MapPush {
        layout(offset = 32) vec4 view;    // xy = map uv under the player, z = uv per radius, w = radius px
        layout(offset = 48) vec4 params;  // x = cos(heading), y = sin(heading), z = unused, w = fog strength
    } pc;

    void main() {
        vec2 p = inUv * 2.0 - 1.0;
        float r = max(abs(p.x), abs(p.y));
        float alpha = clamp((1.0 - r) * pc.view.w + 0.5, 0.0, 1.0);
        vec2 n = vec2(pc.params.x * p.x - pc.params.y * p.y,
                      pc.params.y * p.x + pc.params.x * p.y);
        vec2 uv = pc.view.xy + n * pc.view.z;
        float seen = texture(fogTexture, uv).r;
        float known = smoothstep(0.03, 0.55, seen);
        vec2 inside2 = step(vec2(0.0), uv) * step(uv, vec2(1.0));
        float inside = inside2.x * inside2.y;
        float fog = mix(1.0, 1.0 - known, inside) * pc.params.w;
        outColor = vec4(vec3(0.27, 0.29, 0.33), fog * alpha);
    }
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from spvasm import Module  # noqa: E402


def build():
    m = Module()
    f = m.t_float()
    v2, v4 = m.t_vec(2), m.t_vec(4)

    in_uv = m.variable("Input", v2, "inUv")
    m.decorate(in_uv, "Location", 0)
    out_color = m.variable("Output", v4, "outColor")
    m.decorate(out_color, "Location", 0)

    sampler = m.variable("UniformConstant", m.t_sampled_image2d(), "fogTexture")
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

    m.begin_main("Fragment", [in_uv, out_color], origin_upper_left=True)
    uv0 = m.op("Load", v2, in_uv)
    p2 = m.op("VectorTimesScalar", v2, uv0, c2)
    p = m.op("FSub", v2, p2, vone2)
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
    strength = m.op("CompositeExtract", f, params, 3)

    one_minus_r = m.op("FSub", f, c1, r)
    rim = m.op("FMul", f, one_minus_r, view_w)
    rim_b = m.op("FAdd", f, rim, chalf)
    alpha = m.ext("FClamp", f, rim_b, c0, c1)

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

    tex = m.op("Load", m.t_sampled_image2d(), sampler)
    sample = m.op("ImageSampleImplicitLod", v4, tex, uv)
    seen = m.op("CompositeExtract", f, sample, 0)
    known = m.ext("SmoothStep", f, m.c_float(0.03), m.c_float(0.55), seen)

    s_lo = m.ext("Step", v2, vzero2, uv)
    s_hi = m.ext("Step", v2, uv, vone2)
    s_both = m.op("FMul", v2, s_lo, s_hi)
    in_x = m.op("CompositeExtract", f, s_both, 0)
    in_y = m.op("CompositeExtract", f, s_both, 1)
    inside = m.op("FMul", f, in_x, in_y)

    unknown = m.op("FSub", f, c1, known)
    fog0 = m.ext("FMix", f, c1, unknown, inside)
    fog1 = m.op("FMul", f, fog0, strength)
    fog = m.op("FMul", f, fog1, alpha)

    out = m.op("CompositeConstruct", v4, m.c_float(0.27), m.c_float(0.29), m.c_float(0.33), fog)
    m.store(out_color, out)
    m.end_main()
    return m.assemble()


if __name__ == "__main__":
    dest = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "assets", "embervale_minimap_fog.frag.spv")
    data = build()
    with open(dest, "wb") as fh:
        fh.write(data)
    print(f"wrote {dest} ({len(data)} bytes)")
