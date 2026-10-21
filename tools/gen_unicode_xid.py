#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
import argparse
from pathlib import Path
import regex

PAGE_SHIFT = 8
PAGE_SIZE = 1 << PAGE_SHIFT
WORDS_PER_PAGE = PAGE_SIZE // 64
PAGE_COUNT = (0x110000 + PAGE_SIZE - 1) // PAGE_SIZE


def build_property(pattern: str):
    rx = regex.compile(rf"\A\p{{{pattern}}}\Z")
    pages = []
    for page in range(PAGE_COUNT):
        words = [0] * WORDS_PER_PAGE
        base = page << PAGE_SHIFT
        for i in range(PAGE_SIZE):
            cp = base + i
            if cp > 0x10FFFF:
                break
            if rx.fullmatch(chr(cp)):
                words[i >> 6] |= 1 << (i & 63)
        pages.append(tuple(words))

    unique = []
    unique_index = {}
    page_map = []
    for page in pages:
        idx = unique_index.get(page)
        if idx is None:
            idx = len(unique)
            unique_index[page] = idx
            unique.append(page)
        page_map.append(idx)
    return page_map, unique


def emit_u16_array(name, values):
    out = [f"static const uint16_t {name}[{len(values)}] = {{"]
    for i in range(0, len(values), 16):
        chunk = ", ".join(str(v) for v in values[i:i + 16])
        out.append(f"    {chunk},")
    out.append("};")
    return "\n".join(out)


def emit_pages(name, pages):
    out = [f"static const uint64_t {name}[{len(pages)}][{WORDS_PER_PAGE}] = {{"]
    for page in pages:
        words = ", ".join(f"UINT64_C(0x{v:016x})" for v in page)
        out.append(f"    {{ {words} }},")
    out.append("};")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("output", type=Path)
    args = ap.parse_args()

    start_map, start_pages = build_property("XID_Start")
    cont_map, cont_pages = build_property("XID_Continue")

    text = f"""#ifndef ASTER_UNICODE_XID_H\n#define ASTER_UNICODE_XID_H\n\n#include <stdint.h>\n\n#define ASTER_UNICODE_VERSION \"17.0.0\"\n#define ASTER_UNICODE_PAGE_SHIFT {PAGE_SHIFT}u\n#define ASTER_UNICODE_PAGE_MASK {PAGE_SIZE - 1}u\n#define ASTER_UNICODE_WORDS_PER_PAGE {WORDS_PER_PAGE}u\n\n{emit_u16_array('aster_xid_start_page_map', start_map)}\n\n{emit_pages('aster_xid_start_pages', start_pages)}\n\n{emit_u16_array('aster_xid_continue_page_map', cont_map)}\n\n{emit_pages('aster_xid_continue_pages', cont_pages)}\n\nstatic inline int aster_unicode_xid_start(uint32_t cp) {{\n    if (cp > UINT32_C(0x10ffff)) return 0;\n    uint32_t page = cp >> ASTER_UNICODE_PAGE_SHIFT;\n    uint32_t bit = cp & ASTER_UNICODE_PAGE_MASK;\n    uint16_t idx = aster_xid_start_page_map[page];\n    return (int)((aster_xid_start_pages[idx][bit >> 6] >> (bit & 63u)) & UINT64_C(1));\n}}\n\nstatic inline int aster_unicode_xid_continue(uint32_t cp) {{\n    if (cp > UINT32_C(0x10ffff)) return 0;\n    uint32_t page = cp >> ASTER_UNICODE_PAGE_SHIFT;\n    uint32_t bit = cp & ASTER_UNICODE_PAGE_MASK;\n    uint16_t idx = aster_xid_continue_page_map[page];\n    return (int)((aster_xid_continue_pages[idx][bit >> 6] >> (bit & 63u)) & UINT64_C(1));\n}}\n\n#endif\n"""
    args.output.write_text(text, encoding="utf-8")
    print(f"Unicode {regex.__version__}: start pages={len(start_pages)}, continue pages={len(cont_pages)}")


if __name__ == "__main__":
    main()
