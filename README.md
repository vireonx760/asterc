# AsterASM

AsterASM is a typed low-level language and a native compiler written in C17.
It supports **x86-16, x86-32, x86-64, AArch64 and RISC-V 64**, explicit memory
access, C ABI interoperability, compile-time evaluation and native instruction
blocks. Current version: **1.2.1**.

[Build and install](BUILDING.md) · [Roadmap after 1.2.1](ROADMAP.md)

## Example

```text
module example.answer;

export fn answer() -> u64 @abi(c) {
    return 42;
}
```

```sh
asterc check examples/guide/01_answer.aa
asterc build -O2 --target x86_64 --abi sysv --emit bin -o answer.bin examples/guide/01_answer.aa
asterdump --target x86_64 --format json answer.bin
```

A raw `.bin` image contains code and data at a chosen load address. Use a
loader/emulator to invoke it. Code begins with the first defined function in
compiler emission order; in a single source file, this follows declaration
order. `export` makes a symbol visible but does not select an entry point or
move that function to the start. Declare the intended entry function first
when a loader calls the image's load address. A raw image has no executable
header, entry-point metadata or automatic startup stub. To link a function
into a normal C application, produce an ELF/COFF object with the caller's
target and ABI; the linker resolves the function by its symbol name.

## Language and compiler

* Typed integers, floating point, pointers, arrays, aggregates and generics.
* Modules, compile-time evaluation, macros and explicit loads/stores.
* C-compatible function calls, structures and function pointers.
* Scalar optimization, inlining, atomics and explicit target features.
* `native` bridges and `exact` instruction blocks with register, memory and
  calling-convention verification.
* A function-parallel backend with reusable workers and deterministic output.
* Raw binaries and ELF/COFF/Mach-O object output; a public C SDK.

The encoder generates machine code directly. Rust is used by the optional full
object writer. There is no external assembler in the normal compilation path.

| Target | Calling convention / execution mode |
| --- | --- |
| x86-16 | 386+ code16/flat32, 32-bit pointers and i386 cdecl |
| x86-32 | i386 cdecl |
| x86-64 | SysV AMD64 and Windows x64 |
| AArch64 | AAPCS64 |
| RISC-V 64 | LP64, LP64F and LP64D within supported instruction contracts |

Windows x64 and Linux x86-64 are compiler hosts. AArch64 and RISC-V are cross
compilation targets. Supported operand forms, extensions and ABI combinations
are bounded; AArch32, RV32 and automatic vectorization are not 1.2.1 features.

Use `asterc --help` for CLI options. `-O0`, `-O2` and `-Oz` select optimization
policy; `-j 1` selects one worker, while the default chooses available CPU
capacity. `--target`, `--abi`, `--feature`, `--base` and `--no-red-zone` make
machine/loader requirements explicit.

## Examples

The `examples/guide/` directory contains **100 numbered programs** with English
comments: arithmetic, memory, layouts, generics, atomics, vectors, strings,
binary search, sorting, CRC/FNV, parsers, UTF-8, ring buffers and other algorithms.
Examples 79–100 export `run()` first and return 42 when their checks pass.
Other examples may place a helper first: calling their raw image's load address
invokes that helper, not necessarily the exported function.

| Directory | Purpose |
| --- | --- |
| `examples/guide/modules` | Small imported-function example |
| `examples/modules` | Multi-module application |
| `examples/package_workspace` | Local package dependency and target matrix |
| `examples/c_interop` | C executable calling AsterASM functions and C-string results |
| `examples/minikernel` | Multi-stage BIOS boot, hardware modules, Snake and Tetris |
| `examples/hello` | AsterASM calling external `puts` through the C ABI |
| `examples/cycles` | Deliberately invalid cyclic-import diagnostic example |

Build a standalone kernel image and run it in QEMU:

```sh
python examples/minikernel/build.py --compiler build/release/asterc --run
```

On Windows use `build/release/asterc.exe`. `--smoke` runs headlessly and checks
boot, entry into both games, return to the menu and halt. `--opt O0`, `--opt O2`
and `--opt Oz` select kernel optimization. Output goes into the ignored
`examples/minikernel/build/` directory. QEMU must be on PATH or passed with
`--qemu <path>`.

Boot code is compiled entirely from `.aa` units. Available instructions use
native mnemonics; real-mode forms, segment loads, 32-bit control-register
transfers and far jumps still use small commented `unsafe exact bytes` groups.
The Python builder invokes the compiler, validates sizes and combines outputs.
It does not generate instructions. This example targets a BIOS PC, not UEFI.

Controls: 1 = Snake, 2 = Tetris, WASD = movement/rotation, Q = back,
Q/Escape in the menu = halt. The kernel uses VGA text, PS/2, COM1 and PIT polling.
It has no heap, filesystem, scheduler or interrupt subsystem.

## Tools and tests

`asterc` compiles, `asterdump` inspects binaries, and `aster-lex`, `aster-ast`,
`aster-hir`, `aster-mir` inspect compiler stages. `tools/aster.py` builds package
manifests and `tools/asterfmt.py` formats AsterASM source. Run each with `--help`.
LSP and Tree-sitter are maintained in their separate repositories.

Raw BIN dumps scan the whole image, including any embedded data. ELF/COFF/PE
dumps use executable-section metadata. Unsupported instruction forms remain
explicit `.byte` (x86) or `.word` (AArch64/RISC-V) entries; `--strict` returns
a nonzero status when they occur.

`tests/` retains unit tests, regression fixtures, object/ABI checks, emulation
and concurrency tests. Normal release builds leave tests disabled; enable them
explicitly as described in [BUILDING.md](BUILDING.md). Windows/Linux CI builds
and runs the tests. The example checker is also available:

```sh
python tools/check_examples.py --compiler build/release/asterc --runtime
```

The runtime option requires Python Unicorn. Architecture-specific native,
atomic and vector examples are limited to their applicable targets.

## Licensing

The licenses apply by component, not as a choice for every file:

| Original component | License |
| --- | --- |
| Language/compiler, public C SDK, Rust object writer | Apache-2.0 |
| Examples, compiler tests, build files and documentation | Apache-2.0 |
| `src/cli/asterdump*.[ch]` | MPL-2.0 |
| `src/cli/aster_lex.c`, `aster_ast.c`, `aster_hir.c`, `aster_mir.c` | MPL-2.0 |
| Original utilities in `tools/` | MPL-2.0 |

The original compiler entry `src/cli/asterc.c`, `compiler_pipeline.c`,
`compiler_*.h`, `elf64_fallback.*` and shared compiler libraries remain Apache-2.0.
The default for original files outside the listed MPL components is Apache-2.0.
Individual and third-party notices take precedence.

Full texts: [LICENSE](LICENSE), [Apache-2.0](LICENSES/Apache-2.0.txt),
[MPL-2.0](LICENSES/MPL-2.0.txt). Retained LLVM/Unicode/Rust dependency notices are
in `LICENSES/third-party/`; LLVM-derived instruction records keep the LLVM
exception. `tools/LICENSE` and source SPDX notices identify MPL tool sources.

Binary distributions include the notices and matching sources under
`https://github.com/vireonx760/asterc/tree/v1.2.1`. Modifications to covered tool
files remain under MPL-2.0. Compiling your program does not change its license.

Report reproducible issues at https://github.com/vireonx760/asterc/issues with
source, compiler version, command, target/ABI/features and expected behavior.
