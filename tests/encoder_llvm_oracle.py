"""Compare the encoder's byte corpus against independently assembled LLVM MC.

LLVM is an optional test dependency, never a library/runtime compiler dependency.
No network, shell commands, linking, or execution of generated code is involved.
"""
import argparse
import json
import pathlib
import re
import subprocess
import tempfile


def run(args):
    return subprocess.run(args, check=True, capture_output=True, text=True, timeout=60).stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--test", required=True)
    parser.add_argument("--mc", required=True)
    parser.add_argument("--objcopy", required=True)
    parser.add_argument("--dumper")
    parser.add_argument("--objdump")
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
                if args.dumper:
                    decoded = json.loads(run([args.dumper, '--target', arch,
                                              '--format', 'json', '--strict', str(binary)]))
                    assert decoded['unknown'] == 0, decoded['unknown']
                    if arch != 'x86_64':
                        assert len(decoded['instructions']) * 4 == len(actual)
                        rebuilt_source = root / (arch + '-decoded.s')
                        rebuilt_source.write_text('.text\n' + prefix + '\n'.join(
                            i['assembly'] for i in decoded['instructions']) + '\n', encoding='utf-8')
                        rebuilt_object, rebuilt_binary = root / 'rebuilt.o', root / 'rebuilt.bin'
                        run([args.mc, *flags, '-filetype=obj', str(rebuilt_source), '-o', str(rebuilt_object)])
                        run([args.objcopy, '--only-section=.text', '-O', 'binary', str(rebuilt_object), str(rebuilt_binary)])
                        rebuilt = rebuilt_binary.read_bytes()
                        mismatch = next((i for i, pair in enumerate(zip(actual, rebuilt)) if pair[0] != pair[1]), None)
                        assert rebuilt == actual, (arch, 'dumper roundtrip', mismatch)
                        print(f'{arch}: {len(decoded["instructions"])} decoded instructions reassemble byte-for-byte')
                if arch == 'x86_64' and args.dumper and args.objdump:
                    # The encoder corpus includes compound scalar/FP/atomic
                    # sequences. Every boundary must agree with an independent
                    # disassembler, without falling back to .byte inside code.
                    oracle = run([args.objdump, '--disassemble', '--disassemble-zeroes', str(obj)])
                    boundaries = {}
                    pending = None
                    for line in oracle.splitlines():
                        match = re.match(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2}\s+)+)', line)
                        if match:
                            offset, encoded = int(match[1], 16), bytes.fromhex(match[2])
                            # LLVM prints LOCK as a standalone prefix on some
                            # versions. It belongs to the following instruction.
                            if pending:
                                start, prefix = pending
                                assert offset == start + len(prefix)
                                offset, encoded = start, prefix + encoded
                                pending = None
                            if line[match.end():].strip() == 'lock':
                                pending = offset, encoded
                            else:
                                boundaries[offset] = encoded
                    assert pending is None, pending
                    observed = {i['offset']: bytes.fromhex(i['bytes']) for i in decoded['instructions']}
                    differences = [(offset, observed.get(offset), boundaries.get(offset))
                                   for offset in sorted(observed.keys() | boundaries.keys())
                                   if observed.get(offset) != boundaries.get(offset)]
                    assert decoded['unknown'] == 0 and not differences, differences[:5]
                    assert b''.join(observed.values()) == actual
                    print(f'{arch}: {len(observed)} dumper boundaries match LLVM, zero unknown bytes')
    if failures:
        raise SystemExit("\n".join(failures))


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as exc:
        raise SystemExit(f"Oracle subprocess failed: {exc.cmd}\n{exc.stderr}") from exc
