/* LLVM MC validates the whole constrained sequence, including local branches.
 */
static void atomic_one(unsigned operation, unsigned bits, AstMemoryOrder order,
                       AstMemoryOrder failure, const char *assembly) {
  static const char *const xr[] = {"rax", "r10", "r11", "r8", "r9"};
  static const char *const ar[] = {"x9", "x10", "x11", "x12", "x13", "x14"};
  static const char *const rr[] = {"t0", "t1", "t2", "t3", "t4",
                                   "t5", "t6", "a0", "a1", "a2"};
  unsigned n = target.arch == AST_TARGET_ARCH_X86_64    ? 5
               : target.arch == AST_TARGET_ARCH_AARCH64 ? 6
                                                        : 10;
  const char *const *regs = n == 5 ? xr : n == 6 ? ar : rr;
  AstMachineOpcode op = n == 5   ? AST_MACHINE_X86_LOCKED_ATOMIC
                        : n == 6 ? AST_MACHINE_A64_EXCLUSIVE_ATOMIC
                                 : AST_MACHINE_RISCV_LRSC_ATOMIC;
  AstPhysicalOperand args[11];
  for (unsigned j = 0; j < n; ++j)
    args[j] = G(regs[j], 64);
  args[n] = imm(operation | (bits << 8u), 32);
  AstPhysicalInstructionInfo i = inst(op, n, args, n + 1);
  i.memory_order = order;
  i.failure_memory_order = failure;
  AstPhysicalBlockInfo bb = {0, 1, &i};
  AstEncoderOutput out;
  ++cases;
  int ok = encoder_blocks(target, &bb, 1, 0, &out);
  require(ok, "atomic sequence encodes");
  if (ok)
    report(assembly, &out);
  ast_encoder_output_destroy(&out);
  i.operands[1] = i.operands[0];
  require(!encoder_blocks(target, &bb, 1, 0, &out) && !out.bytes && !out.size,
          "atomic overlapping scratch rejected transactionally");
  ast_encoder_output_destroy(&out);
  i.operands[1] = args[1];
  i.memory_order = AST_MEMORY_ORDER_NONE;
  require(!encoder_blocks(target, &bb, 1, 0, &out) && !out.bytes && !out.size,
          "atomic missing order rejected transactionally");
  ast_encoder_output_destroy(&out);
}
static void atomic_cases(void) {
  if (target.arch == AST_TARGET_ARCH_X86_64) {
    atomic_one(0, 64, AST_MEMORY_ORDER_ACQUIRE, 0, "mov rax, [r11]");
    atomic_one(1, 32, AST_MEMORY_ORDER_SEQ_CST, 0, "xchg dword ptr [r11], r9d");
    atomic_one(3, 8, AST_MEMORY_ORDER_RELAXED, 0,
               "lock xadd byte ptr [r11], r9b; mov al, r9b");
    atomic_one(4, 16, AST_MEMORY_ORDER_ACQ_REL, 0,
               "neg r9w; lock xadd word ptr [r11], r9w; mov ax, r9w");
    atomic_one(8, 64, AST_MEMORY_ORDER_SEQ_CST, AST_MEMORY_ORDER_SEQ_CST,
               "mov rax,r8; lock cmpxchg [r11],r9; sete al");
    atomic_one(7, 32, AST_MEMORY_ORDER_ACQUIRE, 0,
               "mov eax,[r11]; .Latomic_retry$: mov r10d,eax; xor r10d,r9d; "
               "lock cmpxchg [r11],r10d; jne .Latomic_retry$");
  } else if (target.arch == AST_TARGET_ARCH_AARCH64) {
    atomic_one(0, 64, AST_MEMORY_ORDER_ACQUIRE, 0, "ldar x9,[x12]");
    atomic_one(0, 8, AST_MEMORY_ORDER_RELAXED, 0, "ldrb w9,[x12]");
    atomic_one(1, 16, AST_MEMORY_ORDER_SEQ_CST, 0, "stlrh w14,[x12]");
    atomic_one(1, 32, AST_MEMORY_ORDER_RELAXED, 0, "str w14,[x12]");
    atomic_one(3, 8, AST_MEMORY_ORDER_RELEASE, 0,
               ".Latomic_retry$: ldxrb w9,[x12]; add w10,w9,w14; stlxrb "
               "w11,w10,[x12]; cbnz w11,.Latomic_retry$");
    atomic_one(
        8, 64, AST_MEMORY_ORDER_ACQ_REL, AST_MEMORY_ORDER_ACQUIRE,
        ".Latomic_retry$: ldaxr x9,[x12]; cmp x9,x13; b.ne .Latomic_fail$; mov "
        "x10,x14; stlxr w11,x10,[x12]; cbnz w11,.Latomic_retry$; mov w9,#1; b "
        ".Latomic_end$; .Latomic_fail$: clrex; mov w9,#0; .Latomic_end$:");
  } else {
    target.features |= AST_TARGET_FEATURE_RV_A;
    atomic_one(0, 16, AST_MEMORY_ORDER_ACQUIRE, 0, "lhu t0,0(a0); fence r,rw");
    atomic_one(1, 8, AST_MEMORY_ORDER_SEQ_CST, 0,
               "fence rw,w; sb a2,0(a0); fence rw,rw");
    atomic_one(3, 64, AST_MEMORY_ORDER_ACQ_REL, 0, "amoadd.d.aqrl t0,a2,(a0)");
    atomic_one(4, 32, AST_MEMORY_ORDER_RELAXED, 0,
               "neg a2,a2; amoadd.w t0,a2,(a0)");
    atomic_one(8, 32, AST_MEMORY_ORDER_SEQ_CST, AST_MEMORY_ORDER_SEQ_CST,
               "fence rw,rw; mv t4,a0; slli a1,a1,32; srli a1,a1,32; "
               ".Latomic_retry$: lr.w.aqrl t3,(t4); slli t0,t3,32; srli "
               "t0,t0,32; bne t0,a1,.Latomic_fail$; mv t1,a2; sc.w.rl "
               "t2,t1,(t4); bne t2,zero,.Latomic_retry$; li t0,1; j "
               ".Latomic_end$; .Latomic_fail$: li t0,0; .Latomic_end$:");
    atomic_one(
        3, 8, AST_MEMORY_ORDER_RELAXED, 0,
        "andi t4,a0,-4; andi t5,a0,3; slli t5,t5,3; li t6,255; and a1,a1,t6; "
        ".Latomic_retry$: lr.w t3,(t4); srl t0,t3,t5; and t0,t0,t6; add "
        "t1,t0,a2; and t1,t1,t6; sll t1,t1,t5; sll t2,t6,t5; not t2,t2; and "
        "t3,t3,t2; or t1,t3,t1; sc.w t2,t1,(t4); bne t2,zero,.Latomic_retry$");
  }
}
