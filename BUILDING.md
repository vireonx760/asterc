# Building AsterASM 1.2.1

## Requirements

* CMake 3.20+ and a C17 compiler: GCC, Clang or MSVC.
* Ninja, Make or a Visual Studio generator.
* Rust 1.85+ and Cargo for the full object writer. Dependency versions are pinned
  by `object-writer/Cargo.lock`.
* Python 3.11+ for build utilities and Python tests; it is not needed by generated
  programs. CPU execution tests additionally use `unicorn==2.1.4`.

On Windows use a **Visual Studio Developer PowerShell** for MSVC/clang-cl builds.
On Linux install the compiler/build tools and Rust through your chosen package
manager/toolchain. The compiler supplies its own target instruction encoder.

## Release build and installation

```sh
git clone https://github.com/vireonx760/asterc.git
cd asterc
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DASTERASM_BUILD_OBJECT_WRITER=ON
cmake --build build/release --parallel
cmake --install build/release --prefix install
```

The commands also work in Developer PowerShell. Multi-configuration generators
need `--config Release` on build/install. Add the absolute `install/bin` directory
to PATH. Keep the installed object-writer shared library with the installation.
Verify with `asterc --version` and `asterdump --version`.

For a C-only build use `-DASTERASM_BUILD_OBJECT_WRITER=OFF`. This omits the full
Rust object writer and provides a narrower fallback object-output contract.
Raw binary output remains available.

The `release` CMake preset provides a release configuration. Sanitizer/debug
presets enable tests; the release preset leaves tests disabled.

## Tests

Tests and fixtures live in `tests/`. Enable them in a separate build directory:

```sh
python -m pip install unicorn==2.1.4
cmake -S . -B build/test -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DASTERASM_BUILD_OBJECT_WRITER=ON -DASTERASM_REQUIRE_UNICORN=ON
cmake --build build/test --parallel
ctest --test-dir build/test --output-on-failure --parallel 4
```

If Python discovery picks a different interpreter, add
`-DPython3_EXECUTABLE=/absolute/path/to/python` to configuration. The GitHub
workflow builds/tests on Windows and Linux. Optional LLVM, QEMU and Bochs tests
run when their required tools are available; they are development dependencies.

ASan/UBSan: configure with `-DASTERASM_ENABLE_ASAN=ON` and
`-DASTERASM_ENABLE_UBSAN=ON` using a supporting compiler. TSan uses
`-DASTERASM_ENABLE_TSAN=ON` in a separate build and cannot be combined with ASan.
Use `-DASTERASM_WARNINGS_AS_ERRORS=ON` for a strict build.

Check the teaching examples with:

```sh
python tools/check_examples.py --compiler build/release/asterc --runtime
```

Use the `.exe` suffix on Windows. `--skip-objects` supports a compiler built
without the full object writer; omit `--runtime` for compile-only checks.

## Link AsterASM with C

The `examples/c_interop` program checks a byte checksum, static C-string result,
header validation and truncated packets. From the repository root:

Linux x86-64:

```sh
mkdir -p build/c-interop
build/release/asterc build -O2 --target x86_64 --abi sysv --emit obj --format elf -o build/c-interop/packet.o examples/c_interop/packet.aa
cc -std=c17 -Wall -Wextra -Werror examples/c_interop/main.c build/c-interop/packet.o -Wl,-z,noexecstack -o build/c-interop/demo
./build/c-interop/demo
```

Windows x64, Developer PowerShell:

```powershell
New-Item -ItemType Directory -Force build/c-interop | Out-Null
./build/release/asterc.exe build -O2 --target x86_64 --abi win64 --emit obj --format coff -o build/c-interop/packet.obj examples/c_interop/packet.aa
cl /nologo /W4 /WX examples/c_interop/main.c build/c-interop/packet.obj /Fe:build/c-interop/demo.exe /Fo:build/c-interop/main.obj
./build/c-interop/demo.exe
```

Success prints `hello from AsterASM 1.2.1` and returns zero. The C caller's ABI
and object format must match the AsterASM build.

## Package and kernel examples

```sh
python tools/aster.py --manifest-path examples/package_workspace/app/aster.toml --compiler build/release/asterc matrix
python examples/minikernel/build.py --compiler build/release/asterc --smoke
```

The package example has three target configurations. The minikernel command
requires `qemu-system-x86_64`, compiles all boot/kernel units and exercises its
menu/games headlessly. Use `--run` for an interactive display. Its BIOS image is
57856 bytes, with 53248 bytes reserved for the kernel. See README for controls
and the remaining mode-specific byte escapes.

## Embed the C SDK

```cmake
find_package(AsterASM CONFIG REQUIRED)
target_link_libraries(my_compiler_client PRIVATE AsterASM::backend)
```

Set `CMAKE_PREFIX_PATH` to the installation prefix when necessary. Public headers
are in `include/asterasm/`; installed libraries/configuration are under `lib/`.
