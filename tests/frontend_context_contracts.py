#!/usr/bin/env python3
"""Contextual grammar, diagnostics, lexer/AST-preserving formatter properties."""
import argparse
import importlib.util
import json
from pathlib import Path
import random
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
WORDS = ('target','feature','reg','inputs','outputs','to','when','as','in','control','bytes')


def invoke(argv):
    return subprocess.run([str(x) for x in argv], capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=15)


def token_signature(lexer, path):
    p = invoke([lexer,path]); data = path.read_bytes()
    assert p.returncode == 0, p.stdout+p.stderr
    result = []
    for line in p.stdout.splitlines():
        m = re.fullmatch(r'\s*\d+:\s*\d+\s+(\S+)\s+off=(\d+) len=(\d+)',line)
        if m:
            kind, offset, length = m.groups(); offset, length = int(offset),int(length)
            result.append((kind, data[offset:offset+length]))
    assert result, p.stdout
    return result


def fixtures():
    for word in WORDS:
        yield 'names-'+word, f'module context.{word}; type {word}=u64; fn use({word}:{word})->{word} {{ {word}={word}+1; return {word}; }}'
        yield 'generic-'+word, f'module context.{word}; type Box<{word}:type>=ptr<{word}>; type Alias=Box<u64>;'
        yield 'fields-'+word, f'module context.{word}; type Record=struct {{{word}:u64;}}; fn f(a:Record)->u64 {{return a.{word};}}'
        yield 'attributes-'+word, f'module context.{word}; fn f() @{word}({word}) {{let {word}:u64=1;}}'
    for expression in ('(16>>2)', '((9>=3)?4:2)', 'max(16>>2,3)', 'array[8>>1]', 'cast<usize>(8>>1)'):
        yield 'generic-constant-'+expression, 'module context.types; type B<N:const usize>=[u8;N]; type T=B<'+expression+'>;'
    yield 'generic-adjacent-assignment', 'module context.types; type B<N:const usize>=[u8;N]; fn f(){let x:B<4>=0;}'
    yield 'nested-generics', 'module context.types; type Box<T:type>=ptr<T>; type T=Box<Box<ptr<u8>>>;'
    yield 'numeric-member-boundary', 'module context.member; fn f(){return 1 .field;}'
    yield 'unary-address-boundary', 'module context.address; fn f(a:u64){return & &a;}'
    yield 'unicode-whitespace', 'module context.unicode;\u2028fn имя(){\u0085let число=1;\u2029return число;}'
    yield 'language-contexts', '''module context.all;
        target "aarch64-linux-gnu" {feature + "sve2";}
        dispatch fn choose()->u64 {when feature("sve2")=>fast;else=>slow;}
        fn f(target:u64)->u64 {let reg:u64=target; bind reg to reg("arg0");
        unsafe control {reg=reg+1;} comptime for when in 0..4 {let to=when;}
        native aarch64 inputs(reg) outputs(target) {add {dst:target,lhs:reg,rhs:reg};}
        return target;}'''
    for seed in range(128):
        rng = random.Random(seed)
        expr = rng.choice(('a != b','a < b','a >= b','(a >> 1) == b','a && !b'))
        for _ in range(rng.randrange(1,7)): expr = rng.choice(('!('+expr+')','!!('+expr+')','~('+expr+')'))
        yield 'operators-'+str(seed), 'module context.operators;fn f(a:u64,b:u64)->bool {if '+expr+' {return !false;}return !!true;}'
    yield 'comments-numbers', '''module context.comments;
        /* outer /* nested */ kept */ fn f()->u64 { // line kept
        let x=0x12; let y=0o17; let z=0b11; let n=1.0e-3;
        let c='я'; let s=c"value\\n"; let t=b"abc"; return x + y - -z;}'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--parser', required=True); p.add_argument('--lexer', required=True)
    p.add_argument('--formatter', default=str(ROOT/'tools/asterfmt.py'))
    p.add_argument('--compiler'); p.add_argument('--out')
    args = p.parse_args()
    spec = importlib.util.spec_from_file_location('frontend_formatter_contract',args.formatter)
    module = importlib.util.module_from_spec(spec); sys.modules[spec.name]=module; spec.loader.exec_module(module)
    corpus = list(fixtures())
    for directory in ('tests','examples'):
        for path in sorted((ROOT/directory).rglob('*.aa')):
            corpus.append((path.relative_to(ROOT).as_posix(),path.read_text(encoding='utf-8-sig')))
    parsed_count = 0; token_count = 0
    with tempfile.TemporaryDirectory(prefix='aster-frontend-context-') as temporary:
        root=Path(temporary); before=root/'before.aa'; after=root/'after.aa'
        for key, original in corpus:
            before.write_text(original,encoding='utf-8'); formatted=module.format_source(original)
            assert module.format_source(formatted)==formatted,(key,'not idempotent',formatted)
            after.write_text(formatted,encoding='utf-8')
            original_parse=invoke([args.parser,before]); formatted_parse=invoke([args.parser,after])
            if original_parse.returncode == 0:
                assert formatted_parse.returncode==0,(key,formatted_parse.stderr,formatted)
                assert original_parse.stdout==formatted_parse.stdout,(key,'AST mismatch')
                assert token_signature(args.lexer,before)==token_signature(args.lexer,after),(key,'lexer token/value mismatch',formatted)
                parsed_count+=1; token_count+=len(token_signature(args.lexer,after))
            elif not key.startswith(('tests/','examples/')):
                raise AssertionError((key,original_parse.stderr))
        # Logical Unicode columns, all lexer line endings and bounded recovery.
        diagnostic_cases=0
        for newline in ('\n','\r','\r\n','\x85','\u2028','\u2029'):
            for prefix in ('','\t',' /*я*/ '):
                text='module context.diagnostics;'+newline+prefix+'const имя:u64 = ;'+newline+'fn recovered(){}'
                before.write_bytes(text.encode('utf-8'))
                result=invoke([args.parser,before])
                column=len(prefix+'const имя:u64 = ')+1
                assert result.returncode==1 and f':2:{column}: error[parse:expected_expression]' in result.stderr,(repr(newline),prefix,column,result.stderr)
                assert 'recovered' in result.stdout, result.stdout
                if prefix=='\t': assert '  \t' in result.stderr,result.stderr
                diagnostic_cases+=1
        for bad in (b'\xff',b'\xc0\xaf',b'\xe2\x82',b'\xed\xa0\x80',b'\x00'):
            before.write_bytes(b'module context.invalid;\n'+bad+b'\nfn recovered(){}')
            result=invoke([args.parser,before])
            assert result.returncode==1 and ':2:1: error[lex:' in result.stderr,result.stderr
            diagnostic_cases+=1
        if args.compiler:
            code='module context.codegen; export fn f(inputs:u64,outputs:u64)->u64 {let bytes:u64=(inputs>>3)+outputs; if !(bytes!=0) {return 7;} return ~bytes;}'
            before.write_text(code,encoding='utf-8'); after.write_text(module.format_source(code),encoding='utf-8')
            for target in ('x86_16','x86_32','x86_64','aarch64','riscv64'):
                for level in ('0','2','z'):
                    binaries=[]
                    for path in (before,after):
                        raw=root/(path.stem+'.bin')
                        result=invoke([args.compiler,'build','--target',target,'-O'+level,'-j','1','-o',raw,path])
                        assert result.returncode==0,(target,level,result.stderr)
                        binaries.append(raw.read_bytes())
                    assert binaries[0]==binaries[1],(target,level,'machine code changed by formatting')
    report=dict(corpus=len(corpus),valid_roundtrips=parsed_count,tokens=token_count,diagnostic_cases=diagnostic_cases)
    if args.out: Path(args.out).write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print('PASS frontend context/formatter contracts:',json.dumps(report))


if __name__=='__main__':main()
