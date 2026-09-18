"""Tiny SPIR-V 1.0 module builder.

No glslang/shaderc is available in every build environment, so the minimap map
shader is assembled directly from Python. The builder de-duplicates types and
constants and emits the logical-layout sections in the order the SPIR-V spec
requires. Only the handful of opcodes the minimap shaders need are wrapped.
"""
import struct

MAGIC = 0x07230203
GLSL450 = {
    "FAbs": 4, "Floor": 8, "Fract": 10, "Sin": 13, "Cos": 14, "Pow": 26, "Exp": 27, "Sqrt": 31,
    "FMin": 37, "FMax": 40, "FClamp": 43, "FMix": 46, "Step": 48, "SmoothStep": 49,
    "Length": 66, "Distance": 67, "Normalize": 69,
}

# opcodes
OP = dict(
    Source=3, Name=5, MemberName=6, ExtInstImport=11, ExtInst=12, MemoryModelLegacy=14,
    EntryPoint=15, ExecutionMode=16, Capability=17,
    TypeVoid=19, TypeBool=20, TypeInt=21, TypeFloat=22, TypeVector=23, TypeImage=25,
    TypeSampledImage=27, TypeStruct=30, TypePointer=32, TypeFunction=33,
    Constant=43, ConstantComposite=44,
    Function=54, FunctionEnd=56, Variable=59, Load=61, Store=62, AccessChain=65,
    Decorate=71, MemberDecorate=72, VectorShuffle=79, CompositeConstruct=80, CompositeExtract=81,
    ImageSampleImplicitLod=87, ImageSampleExplicitLod=88,
    FNegate=127, FAdd=129, FSub=131, FMul=133, FDiv=136, VectorTimesScalar=142, Dot=148,
    Select=169, FOrdLessThan=184, FOrdGreaterThan=186,
    DPdx=207, DPdy=208, Fwidth=209,
    Label=248, Return=253,
)

# enums
STORAGE = dict(UniformConstant=0, Input=1, Uniform=2, Output=3, Function=7, PushConstant=9)
DECOR = dict(Block=2, BuiltIn=11, Location=30, Binding=33, DescriptorSet=34, Offset=35)
EXEC_MODEL = dict(Vertex=0, Fragment=4)


def _str_words(s):
    b = s.encode("utf-8") + b"\0"
    b += b"\0" * ((4 - len(b) % 4) % 4)
    return list(struct.unpack("<%dI" % (len(b) // 4), b))


class Module:
    def __init__(self):
        self._next = 1
        self.caps, self.imports, self.entries, self.modes = [], [], [], []
        self.debug, self.annotations, self.types = [], [], []
        self.body = []
        self._type_cache = {}
        self._const_cache = {}
        self.glsl = None

    # -- helpers -----------------------------------------------------------
    def id(self):
        v = self._next
        self._next += 1
        return v

    @staticmethod
    def _ins(section, op, *operands):
        words = []
        for o in operands:
            if isinstance(o, (list, tuple)):
                words.extend(o)
            else:
                words.append(o)
        section.append([((len(words) + 1) << 16) | OP[op]] + words)

    def name(self, target, text):
        self._ins(self.debug, "Name", target, _str_words(text))

    # -- types (cached) ----------------------------------------------------
    def _type(self, key, op, *operands):
        if key in self._type_cache:
            return self._type_cache[key]
        rid = self.id()
        self._ins(self.types, op, rid, *operands)
        self._type_cache[key] = rid
        return rid

    def t_void(self): return self._type(("void",), "TypeVoid")
    def t_bool(self): return self._type(("bool",), "TypeBool")
    def t_float(self): return self._type(("float",), "TypeFloat", 32)
    def t_int(self, signed=1): return self._type(("int", signed), "TypeInt", 32, signed)
    def t_vec(self, n): return self._type(("vec", n), "TypeVector", self.t_float(), n)
    def t_bvec(self, n): return self._type(("bvec", n), "TypeVector", self.t_bool(), n)
    def t_func(self, ret): return self._type(("fn", ret), "TypeFunction", ret)
    def t_ptr(self, storage, t): return self._type(("ptr", storage, t), "TypePointer", STORAGE[storage], t)

    def t_image2d(self):
        # float sampled 2D image: dim=2D(1) depth=0 arrayed=0 ms=0 sampled=1 format=Unknown(0)
        return self._type(("image2d",), "TypeImage", self.t_float(), 1, 0, 0, 0, 1, 0)

    def t_sampled_image2d(self):
        return self._type(("sampler2d",), "TypeSampledImage", self.t_image2d())

    def t_struct(self, key, members):
        return self._type(("struct", key), "TypeStruct", *members)

    # -- constants (cached) ------------------------------------------------
    def c_float(self, v):
        key = ("f", struct.pack("<f", v))
        if key not in self._const_cache:
            rid = self.id()
            self._ins(self.types, "Constant", self.t_float(), rid, struct.unpack("<I", struct.pack("<f", v))[0])
            self._const_cache[key] = rid
        return self._const_cache[key]

    def c_int(self, v, signed=1):
        key = ("i", signed, v)
        if key not in self._const_cache:
            rid = self.id()
            self._ins(self.types, "Constant", self.t_int(signed), rid, v & 0xFFFFFFFF)
            self._const_cache[key] = rid
        return self._const_cache[key]

    def c_vec(self, *vals):
        key = ("v", tuple(struct.pack("<f", v) for v in vals))
        if key not in self._const_cache:
            comps = [self.c_float(v) for v in vals]
            rid = self.id()
            self._ins(self.types, "ConstantComposite", self.t_vec(len(vals)), rid, *comps)
            self._const_cache[key] = rid
        return self._const_cache[key]

    # -- globals -----------------------------------------------------------
    def variable(self, storage, pointee, label=None):
        ptr = self.t_ptr(storage, pointee)
        rid = self.id()
        self._ins(self.types, "Variable", ptr, rid, STORAGE[storage])
        if label:
            self.name(rid, label)
        return rid

    def decorate(self, target, decoration, *literals):
        self._ins(self.annotations, "Decorate", target, DECOR[decoration], *literals)

    def member_decorate(self, struct_id, member, decoration, *literals):
        self._ins(self.annotations, "MemberDecorate", struct_id, member, DECOR[decoration], *literals)

    # -- function body -----------------------------------------------------
    def begin_main(self, model, interface, origin_upper_left=False):
        self.caps = []
        self._ins(self.caps, "Capability", 1)  # Shader
        self.glsl = self.id()
        self._ins(self.imports, "ExtInstImport", self.glsl, _str_words("GLSL.std.450"))
        # Required logical-layout instruction (opcode 14); words copied verbatim from the
        # glslang-built frame shader that drivers already accept.
        self._ins(self.imports, "MemoryModelLegacy", 0, 1)
        self._main = self.id()
        self._ins(self.entries, "EntryPoint", EXEC_MODEL[model], self._main, _str_words("main"), *interface)
        if origin_upper_left:
            self._ins(self.modes, "ExecutionMode", self._main, 7)
        self.name(self._main, "main")
        void = self.t_void()
        fn = self.t_func(void)
        self._ins(self.body, "Function", void, self._main, 0, fn)
        self._ins(self.body, "Label", self.id())

    def end_main(self):
        self._ins(self.body, "Return")
        self._ins(self.body, "FunctionEnd")

    def op(self, opname, result_type, *operands):
        rid = self.id()
        self._ins(self.body, opname, result_type, rid, *operands)
        return rid

    def store(self, ptr, value):
        self._ins(self.body, "Store", ptr, value)

    def ext(self, fname, result_type, *args):
        return self.op("ExtInst", result_type, self.glsl, GLSL450[fname], *args)

    # -- output ------------------------------------------------------------
    def assemble(self):
        header = [MAGIC, 0x00010000, 0, self._next, 0]
        sections = [self.caps, self.imports, self.entries, self.modes, self.debug,
                    self.annotations, self.types, self.body]
        words = list(header)
        for sec in sections:
            for ins in sec:
                words.extend(ins)
        return struct.pack("<%dI" % len(words), *words)
