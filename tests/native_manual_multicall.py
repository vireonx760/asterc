#!/usr/bin/env python3
"""G1 Slice 14: call-string keyed verification, link provenance, ABI clobbers.

All cases are actual compiler invocations. A few stable branch cases are also
compared against an external assembler by the preexisting local-call suite.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def scenarios(arch):
    arm = arch == 'aarch64'
    frame = 'sub {dst:sp,lhs:sp,rhs:16};' if arm else 'addi {dst:sp,lhs:sp,rhs:-16};'
    unframe = 'add {dst:sp,lhs:sp,rhs:16};' if arm else 'addi {dst:sp,lhs:sp,rhs:16};'
    sr = 'str {src:x30,offset:0};' if arm else 'sd {src:ra,offset:0};'
    lr = 'ldr {dst:x30,offset:0};' if arm else 'ld {dst:ra,offset:0};'
    sv = 'str {src:x1,offset:8};' if arm else 'sd {src:a1,offset:8};'
    lv = 'ldr {dst:x1,offset:8};' if arm else 'ld {dst:a1,offset:8};'
    add = 'add {dst:x0,lhs:x0,rhs:x1};' if arm else 'add {dst:a0,lhs:a0,rhs:a1};'
    badsaved = 'eor {dst:x19,lhs:x0,rhs:x1};' if arm else 'xor {dst:s1,lhs:a0,rhs:a1};'
    call = lambda name: (f'bl {{target:{name}}};' if arm else f'jal {{dst:ra,target:{name}}};')
    pro = frame + sr
    epi = lr + unframe + 'ret;'
    sub = lambda target: pro + call(target) + epi
    body = lambda name, text: f'label {{name:{name}}}; {text}'
    save_preserved = 'str {src:x19,offset:8};' if arm else 'sd {src:s1,offset:8};'
    load_preserved = 'ldr {dst:x19,offset:8};' if arm else 'ld {dst:s1,offset:8};'
    save_local = 'str {src:x19,offset:0};' if arm else 'sd {src:s1,offset:0};'
    load_local = 'ldr {dst:x19,offset:0};' if arm else 'ld {dst:s1,offset:0};'
    touch_caller = 'ldr {dst:x0,offset:0};' if arm else 'ld {dst:a0,offset:0};'
    tests = [
        # Same leaf visited in two distinct call contexts.  Caller-saved X1/A1
        # must be explicitly reloaded for the second call.
        (True, pro + sv + call('shared') + lv + call('shared') + epi +
         body('shared', add + 'ret;'), 'shared-two-sites'),
        (True, pro + sv + call('first') + lv + call('second') + epi +
         body('first', add + 'ret;') + body('second', add + 'ret;'), 'different-leaves'),
        (True, sub('first') + body('first', sub('second')) +
         body('second', add+'ret;'), 'depth-two'),
        (True, sub('first') + body('first', sub('second')) +
         body('second', sub('third')) + body('third', add+'ret;'), 'depth-three'),
        # SP work inside callee must balance, no implicit fixed frame.
        (True, sub('first') + body('first', frame + unframe + 'ret;'), 'balanced-frame'),
        (True, sub('first') + body('first', frame + save_local +
         badsaved + load_local + unframe + 'ret;'), 'local-preserved-save'),
        (False, frame + sr + save_preserved + call('first') +
         load_preserved + epi + body('first', badsaved + 'ret;'),
         'cannot-use-caller-preserved-slot'),
        (False, sub('first') + body('first', touch_caller + 'ret;'),
         'callee-cannot-read-caller-stack'),
        (False, sub('first') + body('first', unframe + frame + 'ret;'),
         'callee-cannot-borrow-caller-frame'),
        (False, pro + call('shared') + call('shared') + epi +
         body('shared', add+'ret;'), 'read-clobbered-argument'),
        (False, pro + call('first') + add + epi +
         body('first', add+'ret;'), 'caller-saved-read-after-return'),
        (False, pro + call('first') +
         ('add {dst:x0,lhs:x0,rhs:x3};' if arm else
          'add {dst:a0,lhs:a0,rhs:t6};') + epi +
         body('first', 'ret;'), 'caller-saved-temporary-invalidated'),
        (False, sub('first') + body('first', call('second') + 'ret;') +
         body('second', add+'ret;'), 'nested-without-local-RA-save'),
        (False, sub('first') + body('first', pro + call('second') +
         unframe + 'ret;') + body('second', 'ret;'), 'missing-nested-link-restore'),
        (False, sub('first') + body('first', pro + call('second') +
         lr + unframe + 'ret;') + body('second', badsaved + 'ret;'),
         'callee-saved-destroyed'),
        (False, sub('first') + body('first', sub('second')) +
         body('second', sub('third')) + body('third', sub('fourth')) +
         body('fourth', 'ret;'), 'nesting-depth-limit'),
        (False, sub('first') + body('first', sub('first')),
         'direct-recursion-denied'),
        (False, sub('first') + body('first', sub('second')) +
         body('second', sub('first')), 'mutual-recursion-denied'),
        (False, pro + sr + call('first') + epi + body('first', 'ret;'),
         'overwrite-pinned-LR'),
        (False, pro + sv + call('first') + lv + call('first') +
         unframe + 'ret;' + body('first', add+'ret;'),
         'outer-link-not-restored'),
    ]
    # Many independent return continuations must never collapse into the
    # first call's state. These remain below the 4096-statement region cap.
    for count in (8, 32, 128):
        repeated = ''.join(call('shared') + lv for _ in range(count))
        tests.append((True, pro + sv + repeated + epi +
                      body('shared', add + 'ret;'), f'{count}-call-sites'))
    return tests


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--target', choices=['aarch64', 'riscv64'], required=True)
    args = ap.parse_args()
    positive = 0
    for i, (expected, body, tag) in enumerate(scenarios(args.target)):
        with tempfile.TemporaryDirectory(prefix='aster14-multi-') as td:
            src, dst = Path(td) / 'test.aa', Path(td) / 'test.bin'
            src.write_text('module tests.multicall;\nexport fn raw(a:u64,b:u64)->u64 @naked {\n'
                           f'unsafe exact {args.target} {{ {body} }}\n}}\n', encoding='utf8')
            p = subprocess.run([args.compiler, 'build', '--target', args.target,
                                '--emit', 'bin', '-j', '1', '-o', str(dst), str(src)],
                               capture_output=True, text=True, timeout=30)
            if expected:
                if p.returncode or not dst.is_file() or not dst.stat().st_size:
                    raise AssertionError(f'{args.target} [{i}] {tag}: rejected: {p.stderr}')
                positive += 1
            elif p.returncode == 0 or dst.exists():
                raise AssertionError(f'{args.target} [{i}] {tag}: unsound acceptance')
    print(f'{args.target}: {len(scenarios(args.target))} multicall checks '
          f'({positive} accepted, {len(scenarios(args.target)) - positive} rejected)')


if __name__ == '__main__':
    main()
