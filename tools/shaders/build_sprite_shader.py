"""Builds assets/embervale_minimap_sprite.frag.spv (map icons, player marker).

Equivalent GLSL:

    #version 450
    layout(location = 0) in vec2 inUv;            // 0..1 across the sprite quad
    layout(location = 0) out vec4 outColor;
    layout(set = 0, binding = 0) uniform sampler2D atlas;
    layout(push_constant) uniform SpritePush {
        layout(offset = 32) vec4 uvRect;   // u0, v0, u1, v1 of the sprite in the atlas
        layout(offset = 48) vec4 tint;     // rgb multiplier, a = opacity
    } pc;

    void main() {
        vec2 uv = mix(pc.uvRect.xy, pc.uvRect.zw, inUv);
        vec4 c = texture(atlas, uv);
        outColor = vec4(c.rgb * pc.tint.rgb, c.a * pc.tint.a);
    }
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

    sampler = m.variable("UniformConstant", m.t_sampled_image2d(), "atlas")
    m.decorate(sampler, "DescriptorSet", 0)
    m.decorate(sampler, "Binding", 0)

    push_t = m.t_struct("SpritePush", [v4, v4])
    m.name(push_t, "SpritePush")
    m.decorate(push_t, "Block")
    m.member_decorate(push_t, 0, "Offset", 32)
    m.member_decorate(push_t, 1, "Offset", 48)
    pc = m.variable("PushConstant", push_t, "pc")
    ptr_pc_v4 = m.t_ptr("PushConstant", v4)
    k0, k1 = m.c_int(0), m.c_int(1)

    m.begin_main("Fragment", [in_uv, out_color], origin_upper_left=True)

    uv0 = m.op("Load", v2, in_uv)
    rect = m.op("Load", v4, m.op("AccessChain", ptr_pc_v4, pc, k0))
    tint = m.op("Load", v4, m.op("AccessChain", ptr_pc_v4, pc, k1))
    lo = m.op("VectorShuffle", v2, rect, rect, 0, 1)
    hi = m.op("VectorShuffle", v2, rect, rect, 2, 3)
    uv = m.ext("FMix", v2, lo, hi, uv0)

    tex = m.op("Load", m.t_sampled_image2d(), sampler)
    c = m.op("ImageSampleImplicitLod", v4, tex, uv)
    rgb = m.op("VectorShuffle", v3, c, c, 0, 1, 2)
    trgb = m.op("VectorShuffle", v3, tint, tint, 0, 1, 2)
    out_rgb = m.op("FMul", v3, rgb, trgb)
    a = m.op("CompositeExtract", f, c, 3)
    ta = m.op("CompositeExtract", f, tint, 3)
    out_a = m.op("FMul", f, a, ta)
    r = m.op("CompositeExtract", f, out_rgb, 0)
    g = m.op("CompositeExtract", f, out_rgb, 1)
    b = m.op("CompositeExtract", f, out_rgb, 2)
    m.store(out_color, m.op("CompositeConstruct", v4, r, g, b, out_a))
    m.end_main()
    return m.assemble()


if __name__ == "__main__":
    dest = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "assets", "embervale_minimap_sprite.frag.spv")
    data = build()
    with open(dest, "wb") as fh:
        fh.write(data)
    print(f"wrote {dest} ({len(data)} bytes)")
