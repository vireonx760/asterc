"""Compare the encoder's byte corpus against independently assembled LLVM MC.

LLVM is an optional test dependency, never a library/runtime compiler dependency.
No network, shell commands, linking, or execution of generated code is involved.
"""
import argparse
import pathlib
import subprocess
import tempfile


def run(args):
    return subprocess.run(args, check=True, capture_output=True, text=True, timeout=60).stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--test", required=True)
    parser.add_argument("--mc", required=True)
    parser.add_argument("--objcopy", required=True)
    args = parser.parse_args()
    groups = {}
    for index, line in enumerate(run([args.test, "--corpus"]).splitlines()):
        arch, assembly, hexbytes = line.split("\t")
        groups.setdefault(arch, []).append((assembly.replace("$", str(index)), bytes.fromhex(hexbytes)))
    settings = {
        "x86_64": (["-triple=x86_64-unknown-linux-gnu", "-mattr=+popcnt"], ".intel_syntax noprefix\n"),
        "aarch64": (["-triple=aarch64-unknown-linux-gnu"], ""),
        "riscv64": (["-triple=riscv64-unknown-linux-gnu", "-mattr=+m,+a,+f,+d"], ".option norvc\n.option norelax\n"),
    }
    failures = []
    # CTest's working directory is the build tree, including in restricted CI.
    with tempfile.TemporaryDirectory(prefix="aster-encoder-oracle-", dir=pathlib.Path.cwd()) as tmp:
        root = pathlib.Path(tmp)
        for arch, rows in groups.items():
            flags, prefix = settings[arch]
            source, obj, binary = (root / (arch + suffix) for suffix in (".s", ".o", ".bin"))
            source.write_text(".text\n" + prefix + "\n".join(row[0] for row in rows) + "\n", encoding="utf-8")
            run([args.mc, *flags, "-filetype=obj", str(source), "-o", str(obj)])
            run([args.objcopy, "--only-section=.text", "-O", "binary", str(obj), str(binary)])
            actual = binary.read_bytes()
            expected = b"".join(row[1] for row in rows)
            if actual != expected:
                # Show the first discrepancy with its source instruction; a length
                # mismatch must not cause cascades of misleading comparisons.
                first = next((i for i, pair in enumerate(zip(actual, expected)) if pair[0] != pair[1]), min(len(actual), len(expected)))
                pos = 0
                for asm, encoded in rows:
                    if pos + len(encoded) > first:
                        failures.append(f"{arch} at byte {first}: {asm}\n  Aster: {encoded.hex()}\n  LLVM:  {actual[pos:pos+len(encoded)].hex()}")
                        break
                    pos += len(encoded)
                else:
                    failures.append(f"{arch}: total length {len(expected)} != LLVM {len(actual)}")
            else:
                print(f"{arch}: {len(rows)} forms, {len(actual)} bytes match LLVM")
    if failures:
        raise SystemExit("\n".join(failures))


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as exc:
        raise SystemExit(f"Oracle subprocess failed: {exc.cmd}\n{exc.stderr}") from exc
