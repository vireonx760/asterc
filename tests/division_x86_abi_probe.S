/* System V AMD64 ABI probe for raw emitted AsterASM functions.
   uint64_t aster_div_probe(void *fn, uint64_t x, uint64_t y, uint64_t *result)
   Returns 0 if RBX, RBP, R12-R15 survive; 1 otherwise.
   Caller-owned result pointer lives in a stack slot across the indirect call.
   Assembly intentionally does not let the C compiler preserve registers for us. */
.text
.globl aster_div_probe
.type aster_div_probe,@function
aster_div_probe:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $8, %rsp
    movq %rcx, (%rsp)
    movq %rdi, %r11
    movq %rsi, %rdi
    movq %rdx, %rsi
    movabsq $0x1928374655aabbcc, %rbx
    movabsq $0xabcdeff00ff00ff0, %rbp
    movabsq $0x123456789abcdef0, %r12
    movabsq $0xfedcba9876543210, %r13
    movabsq $0x0f1e2d3c4b5a6978, %r14
    movabsq $0x8070605040302010, %r15
    call *%r11
    movq (%rsp), %r10
    movq %rax, (%r10)
    movl $1, %eax
    movabsq $0x1928374655aabbcc, %r10
    cmpq %r10, %rbx
    jne .Laster_div_done
    movabsq $0xabcdeff00ff00ff0, %r10
    cmpq %r10, %rbp
    jne .Laster_div_done
    movabsq $0x123456789abcdef0, %r10
    cmpq %r10, %r12
    jne .Laster_div_done
    movabsq $0xfedcba9876543210, %r10
    cmpq %r10, %r13
    jne .Laster_div_done
    movabsq $0x0f1e2d3c4b5a6978, %r10
    cmpq %r10, %r14
    jne .Laster_div_done
    movabsq $0x8070605040302010, %r10
    cmpq %r10, %r15
    jne .Laster_div_done
    xorl %eax, %eax
.Laster_div_done:
    addq $8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
.size aster_div_probe, .-aster_div_probe
.section .note.GNU-stack,"",@progbits
