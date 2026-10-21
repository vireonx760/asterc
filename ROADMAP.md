# AsterASM roadmap after 1.2.1

Version **1.2.1** is the baseline. The items below are planned work, not
commitments to a release date.

## 1.2.2 — fixes and efficiency

* Fix reported source, ABI and object-output regressions while preserving the
  existing 1.2.x compatibility contract.
* Stabilize AsterBuild Native on large multi-module and standard-library builds,
  including paths with spaces and consistent failure reporting.
* Profile and reduce `check` overhead, peak backend memory and Oz optimization
  cost. Compare one-worker and automatic-worker builds on the same workloads.
* Improve diagnostics for unsupported target/operand/ABI combinations.
* Keep small regression cases for string literals, pointer qualification,
  parallel compilation, object linking and copy/spill cleanup.

## 1.3 — build and debugging workflow

* Incremental module compilation and a dependency-aware artifact cache. Cache
  keys must include imports, compiler version, target, ABI and optimization.
* Initial debug information and better source locations, starting with a tested
  DWARF/ELF path, then Windows support with debugger/linker validation.
* Stable machine-readable diagnostics for build tools, LSP and inspection tools.
* Better copy preferences, spill-slot reuse and CFG placement with measured
  compile time, memory, output size and runtime effects.
* More native x86 mode-specific forms so the BIOS example can replace its
  remaining real/protected-mode byte escapes with typed instruction forms.
* Bring legacy x86 onto more shared backend infrastructure while preserving
  its 386+ instruction requirements and flat32 calling convention.

## Later 1.x

* Expand instruction/operand coverage within the supported target families.
* Improve editor responsiveness and cancellation in independently versioned
  LSP/Tree-sitter tooling.
* Extend standard-library/runtime integration and larger application examples.
* Evaluate additional compiler host environments after native CI is available.

AArch32, RV32, unrestricted scalable-vector ABIs and automatic vectorization
need separate designs and validation; they are not promised 1.2.x features.

## Compatibility

Patch releases preserve source/ABI contracts. New compatible functionality
belongs in minor releases; deliberate breaks require a migration plan.
Verification remains enabled at IR, ABI, allocation and encoding boundaries.
Every supported architecture remains part of regression coverage.

Compiler/language and C SDK stay Apache-2.0; original tools stay MPL-2.0.
Releases use immutable annotated tags: `v1.2.1`, `v1.2.2`, then a suitable
`v1.3.0-rc.1` candidate and `v1.3.0` final release.
