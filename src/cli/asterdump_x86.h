/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ASTERDUMP_X86_H
#define ASTERDUMP_X86_H
#include <stddef.h>
#include <stdint.h>
/* A strict, dependency-free x86-64 subset decoder. Returns zero and consumes
   exactly one byte for an unknown/truncated instruction, guaranteeing progress
   without making up instruction boundaries. Never reads beyond 'available'. */
int dump_x86_decode(const uint8_t *code, size_t available, uint64_t pc,
                    char *text, size_t text_cap, size_t *consumed);
#endif
