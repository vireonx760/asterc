#!/usr/bin/env python3
"""Qualify scalable source operands against an independent LLVM assembler.

The normalized inventory supplies syntax and legal operand domains, not expected
bytes. LLVM independently assembles every case; the dumper must reassemble too.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import tempfile

from native_processor_contracts import assemble, run

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('scalable_generator', ROOT / 'tools/generate_scalable_isa.py')
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)
FEATURES = ('sve2', 'sme', 'sme-i16i64', 'sme-f64f64', 'sme-fa64',
            'sve2-aes', 'sve2-sha3', 'sve2-sm4', 'sve2-bitperm',
            'fp16', 'bf16', 'i8mm', 'f32mm', 'f64mm')
ARCH = '.arch armv9-a+sve2+sme+sme-i16i64+sme-f64f64+sme-fa64+sve2-aes+sve2-sha3+sve2-sm4+sve2-bitperm+fp16+bf16+i8mm+f32mm+f64mm\n'
SUFFIX = {8: 'b', 16: 'h', 32: 's', 64: 'd', 128: 'q'}


def operand_value(t, sample, position):
    kind = t['kind']
    if kind in ('Z', 'P', 'G', 'SP', 'F', 'ZA', 'ZAH', 'ZAV'):
        first, last = t['first'], t['last']
        # x18, LR and SP remain reserved by the manual function contract.
        allowed = [i for i in range(first, last + 1) if kind not in ('G', 'SP') or i not in (18, 30, 31)]
        return allowed[(position if sample == 0 else len(allowed)-1-position if sample == 1 else position+11) % len(allowed)]
    if kind == 'LOGICAL': return (0xff00ff00ff00ff00, 0x8000000080000000, 0xffffffff00000000)[sample]
    if kind == 'FP_PAIR': return t['low' if sample != 1 else 'high'] / 2.0
    if kind == 'FP8': return (1.0, -31.0, 0.125)[sample]
    if kind == 'CPY8': return (-1, 127, -128 if t['width'] == 8 else -256)[sample]
    if kind == 'IMM8': return (1, 255, 128 if t['width'] == 8 else 65280)[sample]
    return (0 if t['low'] <= 0 <= t['high'] else t['low'], t['high'], t['low'])[sample]


def register_name(t, index):
    kind = t['kind']
    if kind == 'Z': return 'z'+str(index)
    if kind == 'P': return 'p'+str(index)
    if kind in ('G', 'SP'): return ('w' if t['width'] == 32 else 'x')+str(index)
    if kind == 'F': return SUFFIX[t['width']]+str(index)
    if kind == 'ZA' and not t['lane']: return 'za'
    return 'za'+str(index)+('h' if kind == 'ZAH' else 'v' if kind == 'ZAV' else '')


def assembly_operand(t, value):
    kind = t['kind']
    if kind in ('Z', 'P', 'G', 'SP', 'F', 'ZA', 'ZAH', 'ZAV'):
        if kind == 'Z' and t['group'] > 1:
            return '{'+', '.join('z'+str((value+i) % 32)+'.'+SUFFIX[t['lane']] for i in range(t['group']))+'}'
        result = ('v'+str(value) if kind == 'F' and t['width'] == 128 else register_name(t, value))
        if t['lane']: result += '.'+SUFFIX[t['lane']]
        if t['extend'] or t['shift']: result += ', '+('lsl', 'uxtw', 'sxtw')[t['extend']]+' #'+str(t['shift'])
        return result
    if t['name'] == 'MatrixTileList':
        return '{'+', '.join('za'+str(i)+'.d' for i in range(8) if value & (1 << i))+'}'
    if t['name'].startswith(('VectorIndex', 'sve_elm_idx_extdup')): return '['+str(value)+']'
    if kind == 'LOGICAL': return '#'+hex(value)
    return '#'+str(value)


def fixture(form, types, sample):
    values = []
    roles = generator.roles(form)
    source = []
    for j, p in enumerate(form['parameters']):
        t = types[p['type']]
        value = values[p['tied']] if p['tied'] >= 0 else operand_value(t, sample, j)
        values.append(value)
        source.append(roles[j]+':'+(register_name(t, value) if t['kind'] in ('Z', 'P', 'G', 'SP', 'F', 'ZA', 'ZAH', 'ZAV') else hex(value) if t['kind'] == 'LOGICAL' else str(value)))
    if form['bits']: source.append('bits:'+str(form['bits']))
    if '/m' in form['assembly'] or '/z' in form['assembly']: source.append('merge:'+str(int('/m' in form['assembly'])))
    if form['input_bits'] != form['bits']: source.append('input_bits:'+str(form['input_bits']))
    group = max((types[p['type']]['group'] for p in form['parameters']), default=1)
    if group > 1: source.append('group:'+str(group))
    extends = {types[p['type']]['extend'] for p in form['parameters']} - {0}
    shifts = {types[p['type']]['shift'] for p in form['parameters']} - {0}
    if extends: source.append('extend:'+('lsl', 'uxtw', 'sxtw')[next(iter(extends))])
    if shifts: source.append('shift:'+str(next(iter(shifts))))
    text = form['name']+' {'+','.join(source)+'};'
    asm = re.sub(r'\$(\d+)', lambda m: assembly_operand(types[form['parameters'][int(m[1])]['type']], values[int(m[1])]), form['assembly'])
    if form['name'] == 'movprfx':
        dst = values[0]
        pred = next((values[j] for j, p in enumerate(form['parameters']) if types[p['type']]['kind'] == 'P'), 0)
        bits = form['bits'] or 32
        rhs = (dst+1) % 32
        text += f'add {{dst:z{dst},pred:p{pred},rhs:z{rhs},bits:{bits}}};'
        asm += f'\nadd z{dst}.{SUFFIX[bits]}, p{pred}/m, z{dst}.{SUFFIX[bits]}, z{rhs}.{SUFFIX[bits]}'
    return text, asm


def wrapper():
    source = ['sub {dst:sp,lhs:sp,rhs:160};']
    asm = ['sub sp,sp,#160']
    for j in range(8):
        source.append(f'str {{src:d{j+8},addr:sp,offset:{j*8}}};')
        asm.append(f'str d{j+8},[sp,#{j*8}]')
    source.append('smstart;'); asm.append('smstart')
    end_source, end_asm = ['smstop;'], ['smstop']
    for j in range(11):
        source.insert(-1, f'str {{src:x{j+19},offset:{64+j*8}}};')
        asm.insert(-1, f'str x{j+19},[sp,#{64+j*8}]')
        end_source.append(f'ldr {{dst:x{j+19},offset:{64+j*8}}};')
        end_asm.append(f'ldr x{j+19},[sp,#{64+j*8}]')
    for j in range(8):
        end_source.append(f'ldr {{dst:d{j+8},addr:sp,offset:{j*8}}};')
        end_asm.append(f'ldr d{j+8},[sp,#{j*8}]')
    end_source += ['add {dst:sp,lhs:sp,rhs:160};ret;']
    end_asm += ['add sp,sp,#160', 'ret']
    return ''.join(source), '\n'.join(asm), ''.join(end_source), '\n'.join(end_asm)


def rejection_cases(prefix, suffix):
    arithmetic = 'add {dst:z0,lhs:z1,rhs:z2,bits:32};'
    bad = [
        ('element-width', 'ptrue {dst:p0,bits:24};'),
        ('predicate-width', 'add {dst:z0,pred:p8,rhs:z2,bits:32};'),
        ('register-range', 'add {dst:z32,lhs:z1,rhs:z2,bits:32};'),
        ('tied-source', 'add {dst:z0,lhs:z1,pred:p0,rhs:z2,bits:32};'),
        ('immediate-range', 'dup {dst:z0,rhs:128,bits:8};'),
        ('rotation', 'cadd {dst:z0,rhs:z1,rotation:45,bits:32};'),
        ('unencodable-mask', 'dupm {dst:z0,rhs:0,bits:64};'),
        ('unencodable-negative-zero', 'fmax {dst:z0,pred:p0,i1:-0.0,bits:32};'),
        ('group', 'tbl {dst:z0,lhs:z1,rhs:z2,bits:8,group:3};'),
        ('za-offset', 'ldr {dst:za,index:w12,slice:1,addr:x0,offset:2};'),
        ('unknown-role', arithmetic.replace('bits:32', 'bits:32,invalid:1')),
        ('platform-register', 'cntw {dst:x18};'),
        ('return-register', 'cntw {dst:x30};'),
        ('scalable-stack', 'ldr {dst:z0,addr:sp,offset:0};'),
        ('matrix-tile', 'fmopa {dst:za4,rows:p0,cols:p0,lhs:z0,rhs:z1,bits:32};'),
        ('prefix-destination', 'movprfx {dst:z0,src:z1};add {dst:z2,pred:p0,rhs:z3,bits:32};'),
        ('prefix-source-alias', 'movprfx {dst:z0,src:z1};add {dst:z0,pred:p0,rhs:z0,bits:32};'),
        ('prefix-predicate', 'movprfx {dst:z0,pred:p0,src:z1,bits:32};add {dst:z0,pred:p1,rhs:z2,bits:32};'),
        ('prefix-adjacency', 'movprfx {dst:z0,src:z1};nop;add {dst:z0,pred:p0,rhs:z2,bits:32};'),
        ('prefix-ending', 'movprfx {dst:z0,src:z1};'),
        ('unknown-streaming-state', 'msr {sys:0xda12,src:x0};'+arithmetic),
    ]
    result = [(key, prefix+body+suffix, FEATURES) for key, body in bad]
    result += [
        ('unsaved-vector', 'add {dst:z8,lhs:z0,rhs:z1,bits:32};ret;', ('sve2',)),
        ('unsaved-mode-transition', 'smstart;smstop;ret;', ('sme',)),
        ('missing-za', 'zero {imm:255};ret;', ('sme',)),
        ('missing-streaming', prefix.replace('smstart;', 'smstart {state:za};')+'fmopa {dst:za0,rows:p0,cols:p0,lhs:z0,rhs:z1,bits:32};'+suffix, FEATURES),
        ('unbalanced-mode', prefix+arithmetic+suffix.replace('smstop;', ''), FEATURES),
        ('missing-feature', arithmetic+'ret;', ()),
        ('nonstreaming-sme-only', arithmetic+'ret;', ('sme',)),
        ('streaming-restricted', prefix+'ldff1w {dst:z0,pred:p0,addr:x0,index:x1,bits:32,shift:2};'+suffix, ('sve2','sme')),
    ]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--dumper', required=True)
    parser.add_argument('--clang', required=True)
    parser.add_argument('--out')
    parser.add_argument('--samples', type=int, default=3, choices=(1, 2, 3))
    parser.add_argument('--batch', type=int, default=256)
    args = parser.parse_args()
    catalog = json.loads((ROOT / 'tools/isa/aarch64_scalable.json').read_text(encoding='utf-8'))
    cases = [(f, sample, *fixture(f, catalog['types'], sample)) for sample in range(args.samples) for f in catalog['forms']]
    prefix, prefix_asm, suffix, suffix_asm = wrapper()
    records = []
    with tempfile.TemporaryDirectory(prefix='aster-scalable-') as directory:
        root = Path(directory); src = root / 'unit.aa'; binary = root / 'unit.bin'
        for offset in range(0, len(cases), args.batch):
            batch = cases[offset:offset+args.batch]
            expected = assemble(args.clang, 'aarch64', ARCH+prefix_asm+'\n'+'\n'.join(c[3] for c in batch)+'\n'+suffix_asm, root)
            for mode in ('native', 'exact', 'unsafe exact'):
                for level in ('0', '2', 'z'):
                    src.write_text('module scalable.contracts; export fn run() @naked { '+mode+' aarch64 {'+prefix+''.join(c[2] for c in batch)+suffix+'}}', encoding='utf-8')
                    cmd = [args.compiler, 'build', '--target', 'aarch64', '-O'+level, '-j', '1', '-o', binary, src]
                    for feature in FEATURES: cmd += ['--feature', feature]
                    try: run(cmd)
                    except AssertionError:
                        print('FAILED BATCH', [(c[0]['id'], c[1]) for c in batch], flush=True)
                        raise
                    actual = binary.read_bytes()
                    assert actual == expected, (offset, mode, level, next((j for j in range(min(len(actual), len(expected))) if actual[j] != expected[j]), None), actual.hex(), expected.hex())
            payload = json.loads(run([args.dumper, '--target', 'aarch64', '--strict', '--format', 'json', binary]).stdout)
            assert payload['unknown'] == 0, (offset, payload)
            dumped = '\n'.join(item.get('assembly', item['mnemonic']) for item in payload['instructions'])
            assert assemble(args.clang, 'aarch64', ARCH+dumped, root) == expected, (offset, dumped)
            records.append(dict(offset=offset, cases=len(batch), sha256=hashlib.sha256(expected).hexdigest()))
            print(f'PASS: {offset+len(batch)}/{len(cases)} scalable LLVM/source/dumper cases, nine modes/levels', flush=True)
        rejects = 0
        for key, body, features in rejection_cases(prefix, suffix):
            for level, mode in (('0','native'), ('2','exact'), ('z','unsafe exact')):
                src.write_text('module scalable.rejected; export fn run() @naked {'+mode+' aarch64 {'+body+'}}', encoding='utf-8')
                binary.unlink(missing_ok=True)
                cmd = [args.compiler, 'build', '--target', 'aarch64', '-O'+level, '-j', '1', '-o', binary, src]
                for feature in features: cmd += ['--feature', feature]
                failed = subprocess.run([str(x) for x in cmd], capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=45)
                assert failed.returncode and failed.stderr and not binary.exists(), (key, level, failed.stdout, failed.stderr)
                rejects += 1
    report = dict(forms=len(catalog['forms']), mnemonics=len({f['name'] for f in catalog['forms']}), cases=len(cases), rejected=rejects, compilations=len(records)*9, records=records)
    if args.out: Path(args.out).write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print('PASS', json.dumps({k:v for k,v in report.items() if k != 'records'}))


if __name__ == '__main__': main()
