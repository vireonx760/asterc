#!/usr/bin/env python3
"""Fail-closed, one-instruction exact manual-code checks for A64/RV64.

Differentially compare accepted code with the *independent* clang assembler,
not the compiler encoder. Rejected native source must never emit an output.
"""
import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

# (expected to pass, instruction body, independent GNU/LLVM assembler spelling)
CASES = {
    'aarch64': [
        # G1 slice07: save/restore incoming link register, only through stack.
        (True, 'sub {dst:sp,lhs:sp,rhs:16}; str {src:x30,offset:0}; ldr {dst:x30,offset:0}; add {dst:sp,lhs:sp,rhs:16}; ret;',
         'sub sp, sp, #16\nstr x30, [sp]\nldr x30, [sp]\nadd sp, sp, #16\nret'),
        (True, 'sub {dst:sp,lhs:sp,rhs:32}; str {src:x30,offset:8}; str {src:x19,offset:0}; ldr {dst:x19,offset:0}; ldr {dst:x30,offset:8}; add {dst:sp,lhs:sp,rhs:32}; ret;',
         'sub sp, sp, #32\nstr x30, [sp, #8]\nstr x19, [sp]\nldr x19, [sp]\nldr x30, [sp, #8]\nadd sp, sp, #32\nret'),
        (False, 'sub {dst:sp,lhs:sp,rhs:16}; str {src:x30,offset:0}; ldr {dst:x30,offset:0}; ldr {dst:x30,offset:0}; add {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (True, 'sub {dst:sp,lhs:sp,rhs:32}; str {src:x30,offset:24}; sub {dst:sp,lhs:sp,rhs:16}; ldr {dst:x30,offset:40}; add {dst:sp,lhs:sp,rhs:16}; add {dst:sp,lhs:sp,rhs:32}; ret;',
         'sub sp, sp, #32\nstr x30, [sp, #24]\nsub sp, sp, #16\nldr x30, [sp, #40]\nadd sp, sp, #16\nadd sp, sp, #32\nret'),
        (False, 'sub {dst:sp,lhs:sp,rhs:32}; str {src:x30,offset:24}; sub {dst:sp,lhs:sp,rhs:16}; ldr {dst:x30,offset:24}; add {dst:sp,lhs:sp,rhs:16}; add {dst:sp,lhs:sp,rhs:32}; ret;', ''),
        (False, 'sub {dst:sp,lhs:sp,rhs:32}; str {src:x30,offset:24}; sub {dst:sp,lhs:sp,rhs:16}; add {dst:sp,lhs:sp,rhs:16}; add {dst:sp,lhs:sp,rhs:32}; ret;', ''),
        (False, 'sub {dst:sp,lhs:sp,rhs:16}; str {src:x30,offset:0}; add {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'sub {dst:sp,lhs:sp,rhs:16}; str {src:x30,offset:0}; str {src:x0,offset:0}; ldr {dst:x30,offset:0}; add {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'sub {dst:sp,lhs:sp,rhs:16}; str {src:x30,offset:0}; ldr {dst:x0,offset:0}; add {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'sub {dst:sp,lhs:sp,rhs:16}; str {src:x0,offset:0}; ldr {dst:x30,offset:0}; add {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'sub {dst:sp,lhs:sp,rhs:16}; str {src:w30,offset:0}; ldr {dst:w30,offset:0}; add {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'sub {dst:sp,lhs:sp,rhs:16}; str {src:x30,offset:0}; ldr {dst:x30,offset:8}; add {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'add {dst:x30,lhs:x0,rhs:x1}; ret;', ''),
        (True, 'add { dst: x0, lhs: x0, rhs: x1 }; ret;', 'add x0, x0, x1\nret'),
        (True, 'sub { dst: w0, lhs: w1, rhs: w2 }; ret;', 'sub w0, w1, w2\nret'),
        (True, 'and { dst: x2, lhs: x0, rhs: x1 }; ret;', 'and x2, x0, x1\nret'),
        (True, 'orr { dst: x0, lhs: x2, rhs: x1 }; ret;', 'orr x0, x2, x1\nret'),
        (True, 'eor { dst: w3, lhs: w2, rhs: w1 }; ret;', 'eor w3, w2, w1\nret'),
        (True, 'add { dst: x0, lhs: x19, rhs: x1 }; ret;', 'add x0, x19, x1\nret'),
        (True, 'add { dst: x1, lhs: x0, rhs: x2 }; sub { dst: x0, lhs: x1, rhs: x3 }; ret;',
         'add x1, x0, x2\nsub x0, x1, x3\nret'),
        (True, 'ret;', 'ret'),
        # Exact, aligned, provably balanced SP adjustments without stack accesses.
        (True, 'sub { dst: sp, lhs: sp, rhs: 16 }; add { dst: sp, lhs: sp, rhs: 16 }; ret;',
         'sub sp, sp, #16\nadd sp, sp, #16\nret'),
        (True, 'sub { dst: sp, lhs: sp, rhs: 4096 }; add { dst: sp, lhs: sp, rhs: 4096 }; ret;',
         'sub sp, sp, #4096\nadd sp, sp, #4096\nret'),
        (True, 'sub { dst: sp, lhs: sp, rhs: 32 }; sub { dst: sp, lhs: sp, rhs: 16 }; add { dst: sp, lhs: sp, rhs: 48 }; ret;',
         'sub sp, sp, #32\nsub sp, sp, #16\nadd sp, sp, #48\nret'),
        # SP-relative stack slots: exact independent assembler and ABI save/restore.
        (True, 'sub {dst:sp,lhs:sp,rhs:32}; str {src:x19,offset:0}; add {dst:x19,lhs:x0,rhs:x1}; ldr {dst:x19,offset:0}; add {dst:sp,lhs:sp,rhs:32}; ret;',
         'sub sp, sp, #32\nstr x19, [sp, #0]\nadd x19, x0, x1\nldr x19, [sp, #0]\nadd sp, sp, #32\nret'),
        (True, 'sub {dst:sp,lhs:sp,rhs:32}; str {src:x0,offset:8}; ldr {dst:x1,offset:8}; add {dst:sp,lhs:sp,rhs:32}; ret;',
         'sub sp, sp, #32\nstr x0, [sp, #8]\nldr x1, [sp, #8]\nadd sp, sp, #32\nret'),
        (True, 'sub {dst:sp,lhs:sp,rhs:32}; str {src:w0,offset:4}; ldr {dst:w1,offset:4}; add {dst:sp,lhs:sp,rhs:32}; ret;',
         'sub sp, sp, #32\nstr w0, [sp, #4]\nldr w1, [sp, #4]\nadd sp, sp, #32\nret'),
        (True, 'sub {dst:sp,lhs:sp,rhs:4096}; str {src:x0,offset:4088}; ldr {dst:x1,offset:4088}; add {dst:sp,lhs:sp,rhs:4096}; ret;',
         'sub sp, sp, #4096\nstr x0, [sp, #4088]\nldr x1, [sp, #4088]\nadd sp, sp, #4096\nret'),
        (True, 'sub {dst:sp,lhs:sp,rhs:32}; str {src:x19,offset:0}; sub {dst:sp,lhs:sp,rhs:16}; add {dst:x19,lhs:x0,rhs:x1}; ldr {dst:x19,offset:16}; add {dst:sp,lhs:sp,rhs:16}; add {dst:sp,lhs:sp,rhs:32}; ret;',
         'sub sp, sp, #32\nstr x19, [sp]\nsub sp, sp, #16\nadd x19, x0, x1\nldr x19, [sp, #16]\nadd sp, sp, #16\nadd sp, sp, #32\nret'),
        (False, 'sub {dst:sp,lhs:sp,rhs:16}; ldr {dst:x0,offset:0}; add {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'sub {dst:sp,lhs:sp,rhs:16}; str {src:x19,offset:0}; add {dst:x19,lhs:x0,rhs:x1}; add {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'sub {dst:sp,lhs:sp,rhs:16}; str {src:x19,offset:0}; str {src:x0,offset:0}; ldr {dst:x19,offset:0}; add {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'sub {dst:sp,lhs:sp,rhs:16}; str {src:x19,offset:0}; str {src:w0,offset:4}; ldr {dst:x19,offset:0}; add {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'sub {dst:sp,lhs:sp,rhs:16}; str {src:x19,offset:0}; ldr {dst:x20,offset:0}; add {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'sub {dst:sp,lhs:sp,rhs:16}; str {src:x19,offset:0}; ldr {dst:x19,offset:8}; add {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'sub {dst:sp,lhs:sp,rhs:16}; str {src:w19,offset:0}; ldr {dst:w19,offset:0}; add {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'sub {dst:sp,lhs:sp,rhs:16}; str {src:x0,offset:16}; add {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'sub {dst:sp,lhs:sp,rhs:16}; str {src:x0,offset:3}; add {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'sub {dst:sp,lhs:sp,rhs:16}; str {src:x0,offset:-8}; add {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'sub {dst:sp,lhs:sp,rhs:16}; str {src:x0,offset:0}; add {dst:sp,lhs:sp,rhs:16}; sub {dst:sp,lhs:sp,rhs:16}; ldr {dst:x1,offset:0}; add {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'add {dst:x19,lhs:x0,rhs:x1}; ret;', ''),
        (False, 'sub {dst:sp,lhs:sp,rhs:16}; ldr {dst:x19,offset:0}; add {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'sub {dst:sp,lhs:sp,rhs:16}; str {src:x19,offset:0}; ldr {dst:x19,offset:0}; ldr {dst:x19,offset:0}; add {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'sub { dst: sp, lhs: sp, rhs: 16 }; ret;', ''),
        (False, 'add { dst: sp, lhs: sp, rhs: 16 }; ret;', ''),
        (False, 'sub { dst: sp, lhs: sp, rhs: 8 }; add { dst: sp, lhs: sp, rhs: 8 }; ret;', ''),
        (False, 'sub { dst: sp, lhs: sp, rhs: 4095 }; add { dst: sp, lhs: sp, rhs: 4095 }; ret;', ''),
        (False, 'sub { dst: sp, lhs: sp, rhs: 16 }; add { dst: sp, lhs: sp, rhs: 32 }; ret;', ''),
        (False, 'sub { dst: sp, lhs: x0, rhs: 16 }; ret;', ''),
        (True, 'sub { dst: x0, lhs: sp, rhs: 16 }; ret;', 'sub x0, sp, #16\nret'),
        (False, 'sub { dst: sp, lhs: sp, rhs: 1048576 }; sub { dst: sp, lhs: sp, rhs: 16 }; add { dst: sp, lhs: sp, rhs: 16 }; add { dst: sp, lhs: sp, rhs: 1048576 }; ret;', ''),
        (False, 'sub { dst: sp, lhs: sp, rhs: 0 }; ret;', ''),
        (False, 'sub { dst: sp, lhs: sp, rhs: -16 }; ret;', ''),
        (False, 'sub { dst: sp, lhs: sp, rhs: 65552 }; ret;', ''),
        (True, 'add { dst: x0, lhs: x1, rhs: 16 }; ret;', 'add x0, x1, #16\nret'),
        (False, 'add { dst: wsp, lhs: wsp, rhs: 16 }; ret;', ''),
        (False, 'sub { dst: sp, lhs: sp, rhs: 16 }; ret; add { dst: sp, lhs: sp, rhs: 16 };', ''),
        (False, 'add { dst: x19, lhs: x0, rhs: x1 }; ret;', ''),
        (False, 'add { dst: x18, lhs: x0, rhs: x1 }; ret;', ''),
        (False, 'add { dst: x30, lhs: x0, rhs: x1 }; ret;', ''),
        (False, 'add { dst: sp, lhs: x0, rhs: x1 }; ret;', ''),
        (True, 'add { dst: x0, lhs: xzr, rhs: x1 }; ret;', 'add x0, xzr, x1\nret'),
        (False, 'add { dst: w0, lhs: x0, rhs: w1 }; ret;', ''),
        (False, 'add { dst: x0, lhs: x0, rhs: x31 }; ret;', ''),
        (False, 'mov { dst: x0, src: x1 }; ret;', ''),
        (False, 'ldr { dst: x0, lhs: x1, rhs: x2 }; ret;', ''),
        (False, 'add { dst: x0, lhs: x0, rhs: x1 };', ''),
        (False, 'ret; add { dst: x0, lhs: x0, rhs: x1 };', ''),
        (True, 'add { dst: x0, lhs: x0, rhs: 12 }; ret;', 'add x0, x0, #12\nret'),
    ],
    'riscv64': [
        # G1 slice07: incoming RA is pinned until restored from its exact slot.
        (True, 'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:ra,offset:0}; ld {dst:ra,offset:0}; addi {dst:sp,lhs:sp,rhs:16}; ret;',
         'addi sp, sp, -16\nsd ra, 0(sp)\nld ra, 0(sp)\naddi sp, sp, 16\nret'),
        (True, 'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:x1,offset:0}; ld {dst:x1,offset:0}; addi {dst:sp,lhs:sp,rhs:16}; ret;',
         'addi sp, sp, -16\nsd x1, 0(sp)\nld x1, 0(sp)\naddi sp, sp, 16\nret'),
        (True, 'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:ra,offset:0}; ld {dst:x1,offset:0}; addi {dst:sp,lhs:sp,rhs:16}; ret;',
         'addi sp, sp, -16\nsd ra, 0(sp)\nld x1, 0(sp)\naddi sp, sp, 16\nret'),
        (True, 'addi {dst:sp,lhs:sp,rhs:-32}; sd {src:ra,offset:8}; sd {src:s0,offset:0}; ld {dst:s0,offset:0}; ld {dst:ra,offset:8}; addi {dst:sp,lhs:sp,rhs:32}; ret;',
         'addi sp, sp, -32\nsd ra, 8(sp)\nsd s0, 0(sp)\nld s0, 0(sp)\nld ra, 8(sp)\naddi sp, sp, 32\nret'),
        (True, 'addi {dst:sp,lhs:sp,rhs:-32}; sd {src:ra,offset:24}; addi {dst:sp,lhs:sp,rhs:-16}; ld {dst:ra,offset:40}; addi {dst:sp,lhs:sp,rhs:16}; addi {dst:sp,lhs:sp,rhs:32}; ret;',
         'addi sp, sp, -32\nsd ra, 24(sp)\naddi sp, sp, -16\nld ra, 40(sp)\naddi sp, sp, 16\naddi sp, sp, 32\nret'),
        (False, 'addi {dst:sp,lhs:sp,rhs:-32}; sd {src:ra,offset:24}; addi {dst:sp,lhs:sp,rhs:-16}; ld {dst:ra,offset:24}; addi {dst:sp,lhs:sp,rhs:16}; addi {dst:sp,lhs:sp,rhs:32}; ret;', ''),
        (False, 'addi {dst:sp,lhs:sp,rhs:-32}; sd {src:ra,offset:24}; addi {dst:sp,lhs:sp,rhs:32}; ret;', ''),
        (False, 'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:ra,offset:0}; ld {dst:x1,offset:0}; ld {dst:ra,offset:0}; addi {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:ra,offset:0}; ld {dst:ra,offset:0}; ld {dst:ra,offset:0}; addi {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:ra,offset:0}; addi {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:ra,offset:0}; sd {src:a0,offset:0}; ld {dst:ra,offset:0}; addi {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:ra,offset:0}; ld {dst:a0,offset:0}; addi {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:a0,offset:0}; ld {dst:ra,offset:0}; addi {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:ra,offset:0}; ld {dst:ra,offset:8}; addi {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'add {dst:ra,lhs:a0,rhs:a1}; ret;', ''),
        (True, 'add { dst: a0, lhs: a0, rhs: a1 }; ret;', 'add a0, a0, a1\nret'),
        (True, 'sub { dst: x10, lhs: x11, rhs: x12 }; ret;', 'sub x10, x11, x12\nret'),
        (True, 'and { dst: t0, lhs: a0, rhs: t1 }; ret;', 'and t0, a0, t1\nret'),
        (True, 'or { dst: a0, lhs: t3, rhs: t4 }; ret;', 'or a0, t3, t4\nret'),
        (True, 'xor { dst: a1, lhs: a0, rhs: a2 }; ret;', 'xor a1, a0, a2\nret'),
        (True, 'addi { dst: a0, lhs: a1, rhs: -2048 }; ret;', 'addi a0, a1, -2048\nret'),
        (True, 'addi { dst: a0, lhs: a1, rhs: 2047 }; ret;', 'addi a0, a1, 2047\nret'),
        (True, 'add { dst: a0, lhs: s0, rhs: a1 }; ret;', 'add a0, s0, a1\nret'),
        (True, 'addi { dst: a0, lhs: a0, rhs: 0x7ff }; ret;', 'addi a0, a0, 2047\nret'),
        (True, 'add { dst: a2, lhs: a0, rhs: a1 }; xor { dst: a0, lhs: a2, rhs: a1 }; ret;',
         'add a2, a0, a1\nxor a0, a2, a1\nret'),
        (True, 'ret;', 'ret'),
        (True, 'addi { dst: sp, lhs: sp, rhs: -16 }; addi { dst: sp, lhs: sp, rhs: 16 }; ret;',
         'addi sp, sp, -16\naddi sp, sp, 16\nret'),
        (True, 'addi { dst: sp, lhs: sp, rhs: -2032 }; addi { dst: sp, lhs: sp, rhs: 2032 }; ret;',
         'addi sp, sp, -2032\naddi sp, sp, 2032\nret'),
        (True, 'addi { dst: sp, lhs: sp, rhs: -32 }; addi { dst: sp, lhs: sp, rhs: -16 }; addi { dst: sp, lhs: sp, rhs: 48 }; ret;',
         'addi sp, sp, -32\naddi sp, sp, -16\naddi sp, sp, 48\nret'),
        # RV64 base-I stack stores/loads, callee-save proof and slot lifetime.
        (True, 'addi {dst:sp,lhs:sp,rhs:-32}; sd {src:s0,offset:0}; add {dst:s0,lhs:a0,rhs:a1}; ld {dst:s0,offset:0}; addi {dst:sp,lhs:sp,rhs:32}; ret;',
         'addi sp, sp, -32\nsd s0, 0(sp)\nadd s0, a0, a1\nld s0, 0(sp)\naddi sp, sp, 32\nret'),
        (True, 'addi {dst:sp,lhs:sp,rhs:-32}; sd {src:a0,offset:8}; ld {dst:a1,offset:8}; addi {dst:sp,lhs:sp,rhs:32}; ret;',
         'addi sp, sp, -32\nsd a0, 8(sp)\nld a1, 8(sp)\naddi sp, sp, 32\nret'),
        (True, 'addi {dst:sp,lhs:sp,rhs:-2032}; sd {src:a0,offset:2024}; ld {dst:a1,offset:2024}; addi {dst:sp,lhs:sp,rhs:2032}; ret;',
         'addi sp, sp, -2032\nsd a0, 2024(sp)\nld a1, 2024(sp)\naddi sp, sp, 2032\nret'),
        (True, 'addi {dst:sp,lhs:sp,rhs:-32}; sd {src:s0,offset:0}; addi {dst:sp,lhs:sp,rhs:-16}; xor {dst:s0,lhs:a0,rhs:a1}; ld {dst:s0,offset:16}; addi {dst:sp,lhs:sp,rhs:16}; addi {dst:sp,lhs:sp,rhs:32}; ret;',
         'addi sp, sp, -32\nsd s0, 0(sp)\naddi sp, sp, -16\nxor s0, a0, a1\nld s0, 16(sp)\naddi sp, sp, 16\naddi sp, sp, 32\nret'),
        (False, 'addi {dst:sp,lhs:sp,rhs:-16}; ld {dst:a0,offset:0}; addi {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:s0,offset:0}; addi {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:s0,offset:0}; sd {src:a0,offset:0}; ld {dst:s0,offset:0}; addi {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:s0,offset:0}; ld {dst:s1,offset:0}; addi {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:s0,offset:0}; ld {dst:s0,offset:8}; addi {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:a0,offset:16}; addi {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:a0,offset:-8}; addi {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:a0,offset:1}; addi {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:a0,offset:0}; addi {dst:sp,lhs:sp,rhs:16}; addi {dst:sp,lhs:sp,rhs:-16}; ld {dst:a1,offset:0}; addi {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'add {dst:s0,lhs:a0,rhs:a1}; ret;', ''),
        (False, 'addi {dst:sp,lhs:sp,rhs:-16}; ld {dst:s0,offset:0}; addi {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:s0,offset:0}; ld {dst:s0,offset:0}; ld {dst:s0,offset:0}; addi {dst:sp,lhs:sp,rhs:16}; ret;', ''),
        (False, 'addi { dst: sp, lhs: sp, rhs: -16 }; ret;', ''),
        (False, 'addi { dst: sp, lhs: sp, rhs: 16 }; ret;', ''),
        (False, 'addi { dst: sp, lhs: sp, rhs: -8 }; addi { dst: sp, lhs: sp, rhs: 8 }; ret;', ''),
        (False, 'addi { dst: sp, lhs: sp, rhs: -32 }; addi { dst: sp, lhs: sp, rhs: 16 }; ret;', ''),
        (False, 'addi { dst: sp, lhs: sp, rhs: -2048 }; addi { dst: sp, lhs: sp, rhs: 2048 }; ret;', ''),
        (False, 'addi { dst: sp, lhs: a0, rhs: -16 }; ret;', ''),
        (True, 'addi { dst: a0, lhs: sp, rhs: -16 }; ret;', 'addi a0, sp, -16\nret'),
        (False, 'addi { dst: sp, lhs: sp, rhs: 0 }; ret;', ''),
        (False, 'add { dst: sp, lhs: sp, rhs: a0 }; ret;', ''),
        (False, 'addi { dst: x2, lhs: x2, rhs: -16 }; ret;', ''),
        (False, 'addi { dst: s0, lhs: a0, rhs: a1 }; ret;', ''),
        (False, 'add { dst: gp, lhs: a0, rhs: a1 }; ret;', ''),
        (False, 'add { dst: tp, lhs: a0, rhs: a1 }; ret;', ''),
        (False, 'add { dst: sp, lhs: a0, rhs: a1 }; ret;', ''),
        (False, 'add { dst: ra, lhs: a0, rhs: a1 }; ret;', ''),
        (True, 'add { dst: zero, lhs: a0, rhs: a1 }; ret;', 'add zero, a0, a1\nret'),
        (False, 'addi { dst: a0, lhs: a0, rhs: 2048 }; ret;', ''),
        (False, 'addi { dst: a0, lhs: a0, rhs: -2049 }; ret;', ''),
        (False, 'mul { dst: a0, lhs: a0, rhs: a1 }; ret;', ''),
        (False, 'mv { dst: a0, src: a1 }; ret;', ''),
        (False, 'ret; add { dst: a0, lhs: a0, rhs: a1 };', ''),
        (False, 'add { dst: a0, lhs: a0, rhs: a1 };', ''),
    ],
}


def assemble(clang, arch, asm, temp_dir):
    text = pathlib.Path(temp_dir) / 'ref.s'
    obj = pathlib.Path(temp_dir) / 'ref.o'
    text.write_text('.text\n' + asm + '\n', encoding='ascii')
    triple = 'aarch64-none-elf' if arch == 'aarch64' else 'riscv64-none-elf'
    argv = [clang, '-target', triple]
    if arch == 'riscv64':
        argv += ['-march=rv64i', '-mabi=lp64']
    proc = subprocess.run(argv + ['-c', str(text), '-o', str(obj)], capture_output=True, text=True, timeout=20)
    if proc.returncode:
        raise AssertionError('reference assembler: ' + proc.stderr)
    proc = subprocess.run(['readelf', '-x', '.text', str(obj)], capture_output=True, text=True, timeout=20)
    if proc.returncode:
        raise AssertionError('readelf: ' + proc.stderr)
    result = bytearray()
    for line in proc.stdout.splitlines():
        if not re.match(r'^\s*0x[0-9a-fA-F]+\s', line):
            continue
        for part in line.split()[1:]:
            if re.fullmatch(r'[0-9a-fA-F]{8}', part):
                result.extend(bytes.fromhex(part))
            else:
                break
    if not result:
        raise AssertionError('empty reference assembler output')
    return bytes(result)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--target', choices=sorted(CASES), required=True)
    ap.add_argument('--clang', default=None)
    args = ap.parse_args()
    for index, (expected, body, asm) in enumerate(CASES[args.target]):
        with tempfile.TemporaryDirectory(prefix='aster-g1-manual-') as td:
            root = pathlib.Path(td)
            source = root / 'example.aa'
            dest = root / 'example.bin'
            source.write_text('module tests.native_exact;\nexport fn raw(a: u64, b: u64) -> u64 @naked {\n'
                              f'unsafe exact {args.target} {{ {body} }}\n}}\n', encoding='utf-8')
            run = subprocess.run([args.compiler, 'build', '--target', args.target, '--emit', 'bin',
                                  '-j', '1', '-o', str(dest), str(source)],
                                 capture_output=True, text=True, timeout=35)
            if expected:
                if run.returncode or not dest.is_file():
                    raise AssertionError(f'{args.target}[{index}] should compile:\n{run.stdout}{run.stderr}')
                actual = dest.read_bytes()
                if not actual or len(actual) != 4 * (asm.count('\n') + 1):
                    raise AssertionError(f'{args.target}[{index}] wrong exact output {actual.hex()}')
                if args.clang:
                    reference = assemble(args.clang, args.target, asm, td)
                    if actual != reference:
                        raise AssertionError(f'{args.target}[{index}] expected {reference.hex()} got {actual.hex()}')
            elif not run.returncode:
                raise AssertionError(f'{args.target}[{index}] invalid native form compiled: {body}')
    print(f'{args.target}: {len(CASES[args.target])} exact/native cases passed')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (AssertionError, subprocess.TimeoutExpired) as exc:
        sys.stderr.write(str(exc) + '\n')
        sys.exit(1)
