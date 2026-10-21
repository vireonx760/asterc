/* RVV 1.0: VL/VTYPE remain architectural state selected by the programmer. */
#define AST_RVV(id, text, shape, funct6, funct3)                               \
  AST_VECTOR_FORM(RV_##id, text, RISCV64, RV_##shape,                          \
                  ((funct6 << 26u) | (funct3 << 12u) | 0x57u),                 \
                  AST_TARGET_FEATURE_RV_V, 0)
#define AST_RVV_PAIR(id, text, funct6, group)                                  \
  AST_RVV(id##_VV, text ".vv", VV, funct6, group)                              \
  AST_RVV(id##_VX, text ".vx", VX, funct6, (group | 4u))
#define AST_RVV_TRIPLE(id, text, funct6)                                       \
  AST_RVV_PAIR(id, text, funct6, 0u)                                           \
  AST_RVV(id##_VI, text ".vi", VI, funct6, 3u)
AST_RVV_TRIPLE(VADD, "vadd", 0x00u)
AST_RVV_PAIR(VSUB, "vsub", 0x02u, 0u)
AST_RVV_TRIPLE(VAND, "vand", 0x09u)
AST_RVV_TRIPLE(VOR, "vor", 0x0au)
AST_RVV_TRIPLE(VXOR, "vxor", 0x0bu)
AST_RVV_PAIR(VMINU, "vminu", 0x04u, 0u)
AST_RVV_PAIR(VMIN, "vmin", 0x05u, 0u)
AST_RVV_PAIR(VMAXU, "vmaxu", 0x06u, 0u)
AST_RVV_PAIR(VMAX, "vmax", 0x07u, 0u)
AST_RVV_PAIR(VMUL, "vmul", 0x25u, 2u)
AST_RVV_PAIR(VMULHU, "vmulhu", 0x24u, 2u)
AST_RVV_PAIR(VMULHSU, "vmulhsu", 0x26u, 2u)
AST_RVV_PAIR(VMULH, "vmulh", 0x27u, 2u)
AST_RVV_PAIR(VDIVU, "vdivu", 0x20u, 2u)
AST_RVV_PAIR(VDIV, "vdiv", 0x21u, 2u)
AST_RVV_PAIR(VREMU, "vremu", 0x22u, 2u)
AST_RVV_PAIR(VREM, "vrem", 0x23u, 2u)
AST_RVV_TRIPLE(VSADDU, "vsaddu", 0x20u)
AST_RVV_TRIPLE(VSADD, "vsadd", 0x21u)
AST_RVV_PAIR(VSSUBU, "vssubu", 0x22u, 0u)
AST_RVV_PAIR(VSSUB, "vssub", 0x23u, 0u)
#define AST_RVV_SHIFT(id, text, funct6)                                        \
  AST_RVV_PAIR(id, text, funct6, 0u)                                           \
  AST_RVV(id##_VI, text ".vi", UIMM, funct6, 3u)
AST_RVV_SHIFT(VSLL, "vsll", 0x25u)
AST_RVV_SHIFT(VSRL, "vsrl", 0x28u)
AST_RVV_SHIFT(VSRA, "vsra", 0x29u)
#undef AST_RVV_SHIFT
#undef AST_RVV_TRIPLE
#undef AST_RVV_PAIR
#undef AST_RVV
AST_VECTOR_FORM(RV_VSETVLI, "vsetvli", RISCV64, RV_CONFIG, 0x7057u,
                AST_TARGET_FEATURE_RV_V, 0)
AST_VECTOR_FORM(RV_VSETIVLI, "vsetivli", RISCV64, RV_ICONFIG, 0xc0007057u,
                AST_TARGET_FEATURE_RV_V, 0)
AST_VECTOR_FORM(RV_VSETVL, "vsetvl", RISCV64, RV_RCONFIG, 0x80007057u,
                AST_TARGET_FEATURE_RV_V, 0)
#define AST_RVV_MEMORY(bits, width)                                            \
  AST_VECTOR_FORM(RV_VLE##bits, "vle" #bits ".v", RISCV64, RV_LOAD,            \
                  ((width << 12u) | 7u), AST_TARGET_FEATURE_RV_V, 0)           \
  AST_VECTOR_FORM(RV_VSE##bits, "vse" #bits ".v", RISCV64, RV_STORE,           \
                  ((width << 12u) | 0x27u), AST_TARGET_FEATURE_RV_V, 0)
AST_RVV_MEMORY(8, 0u)
AST_RVV_MEMORY(16, 5u)
AST_RVV_MEMORY(32, 6u)
AST_RVV_MEMORY(64, 7u)
#undef AST_RVV_MEMORY
