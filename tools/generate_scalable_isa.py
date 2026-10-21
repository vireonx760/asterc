#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Normalize pinned LLVM A64 records into the standalone scalable ISA catalog.

The compiler consumes checked-in C tables, without TableGen or an assembler.
The upstream record dump is a development input, never a runtime dependency.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PREDICATES = {
    'HasSVEorSME': ('', 'SVE|SME'), 'HasSVE2orSME': ('', 'SVE2|SME'),
    'HasSVE': ('SVE', ''), 'HasSVE2': ('SVE2', ''), 'HasSME': ('SME', ''),
    'HasSVE2p1_or_HasSME': ('SME', ''),
    'HasSMEI16I64': ('SME_I16I64', ''), 'HasSMEF64F64': ('SME_F64F64', ''),
    'HasSVE2BitPerm': ('SVE2_BITPERM', ''), 'HasSVE2AES': ('SVE2_AES', ''),
    'HasSVE2SM4': ('SVE2_SM4', ''), 'HasSVE2SHA3': ('SVE2_SHA3', ''),
    'HasBF16': ('BF16', ''), 'HasFullFP16': ('FP16', ''),
    'HasMatMulInt8': ('I8MM', ''), 'HasMatMulFP32': ('F32MM', ''),
    'HasMatMulFP64': ('F64MM', ''), 'HasNEONorSME': ('', ''),
}


def operand_type(name, records):
    d = records[name]
    printer = d.get('PrintMethod', '')
    pred = records.get((d.get('ParserMatchClass') or {}).get('def'), {}).get('PredicateMethod', '') or ''
    t = dict(name=name, kind='', width=0, lane=0, first=0, last=31,
             group=1, extend=0, shift=0, low=0, high=0, scale=1, transform=0,
             printer=printer)
    reg = (d.get('RegClass') or {}).get('def', name if re.fullmatch(r'(?:GPR(?:32|64)(?:sp)?|FPR(?:8|16|32|64)|V128)',name) else '')
    if reg:
        if re.fullmatch(r'FPR(?:8|16|32|64)asZPR',name):
            t['kind'] = 'F'; t['width'] = int(re.search(r'\d+',name)[0])
        elif reg.startswith('ZPR'):
            t['kind'] = 'Z'
            m = re.search(r"printSVERegOp<'(.)'>|printTypedVectorList<0,'(.)'>", printer)
            if m: t['lane'] = {'b':8,'h':16,'s':32,'d':64,'q':128}[next(x for x in m.groups() if x)]
            if reg.endswith('_3b'): t['last'] = 7
            if reg.endswith('_4b'): t['last'] = 15
            if re.fullmatch(r'ZPR[234]', reg): t['group'] = int(reg[-1])
        elif reg.startswith('PPR'):
            t['kind'] = 'P'; t['last'] = 7 if reg.endswith('_3b') else 15
            m = re.search(r"printSVERegOp<'(.)'>", printer)
            if m: t['lane'] = {'b':8,'h':16,'s':32,'d':64}[m[1]]
        elif reg.startswith('GPR') or reg.startswith('MatrixIndexGPR'):
            t['kind'] = 'G'; t['width'] = 32 if '32' in reg else 64
            if printer == 'printGPR64as32': t['width'] = 32
            if reg.startswith('MatrixIndexGPR'):
                t['first'], t['last'] = map(int,re.search(r'_(\d+)_(\d+)$',reg).groups())
            elif reg.endswith('sp'): t['kind'] = 'SP'
            elif reg.endswith('common'): t['last'] = 30
        elif reg.startswith('FPR') or reg == 'V128':
            t['kind'] = 'F'; t['width'] = int(re.search(r'\d+',reg)[0])
            if 'asZPR' in name:
                t['kind'] = 'Z'; t['lane'] = int(re.search(r'\d+',name)[0]); t['width'] = 0
        elif reg.startswith('MPR'):
            t['kind'] = 'ZA'; t['last'] = 0
            m = re.search(r'\d+',reg)
            if m: t['lane'] = int(m[0]); t['last'] = t['lane']//8-1
            if name.startswith('TileVectorOpH'): t['kind'] = 'ZAH'
            if name.startswith('TileVectorOpV'): t['kind'] = 'ZAV'
        else:
            raise ValueError(('unclassified register type', name, reg))
        m = re.search(r"printRegWithShiftExtend<(true|false), (\d+), '([wx])', (?:'([sd])'|0)>",printer)
        if m:
            t['shift'] = (int(m[2])//8).bit_length()-1
            t['extend'] = 2 if m[1]=='true' else 1 if m[3]=='w' else 0
            if t['kind']=='Z': t['lane'] = {'s':32,'d':64}.get(m[4],0)
            elif t['kind']=='G': t['width'] = 32 if m[3]=='w' else 64
        return t
    t['kind'] = 'I'; t['last'] = 0
    m = re.fullmatch(r'is(?:ImmInRange|VectorIndex)<\s*(-?\d+),\s*(-?\d+)\s*>',pred)
    if m:
        t['low'],t['high'] = map(int,m.groups())
    elif (m := re.fullmatch(r'is([SU])Imm(?:Scaled)?<\s*(\d+)(?:,\s*(\d+))?\s*>',pred)):
        bits = int(m[2]); t['scale'] = int(m[3] or 1)
        t['low'] = -(1 << (bits-1)) if m[1]=='S' else 0
        t['high'] = (1 << (bits-(m[1]=='S')))-1
        t['low'] *= t['scale']; t['high'] *= t['scale']
    elif pred.startswith('isComplexRotation'):
        step,bias=map(int,re.findall(r'\d+',pred)); t.update(kind='ROT',low=bias,high=bias+360-step,scale=step)
    elif name == 'sve_incdec_imm': t.update(low=1,high=16)
    elif name == 'sve_pred_enum': t.update(low=0,high=31)
    elif name == 'sve_prfop': t.update(low=0,high=15)
    elif name == 'MatrixTileList': t.update(low=0,high=255)
    elif name == 'logical_imm64': t['kind'] = 'LOGICAL'
    elif name.startswith('addsub_imm8_') or name.startswith('cpy_imm8_'):
        t['kind'] = 'CPY8' if name.startswith('cpy') else 'IMM8'
        t['width'] = int(re.search(r'_i(\d+)$',name)[1])
    elif name.startswith('sve_fpimm_'):
        t['kind'] = 'FP_PAIR'; t['low'],t['high'] = {'sve_fpimm_half_one':(1,2),'sve_fpimm_half_two':(1,4),'sve_fpimm_zero_one':(0,2)}[name]
    elif re.fullmatch(r'fpimm(16|32|64)',name): t['kind'] = 'FP8';t['width'] = int(name[5:])
    else: raise ValueError(('unclassified immediate type',name,pred))
    encoder=d.get('EncoderMethod','')
    if 'getVecShiftR' in encoder: t['transform'] = int(re.search(r'\d+',encoder)[0])
    if 'getVecShiftL' in encoder: t['transform'] = -int(re.search(r'\d+',encoder)[0])
    if encoder=='getSVEIncDecImm': t['transform'] = 1; t['kind'] = 'INC'
    return t


def normalize(records):
    selected={}
    for name,d in records.items():
        if not isinstance(d,dict) or d.get('isPseudo',1) or not d.get('AsmString'): continue
        predicates=[p['def'] for p in d.get('Predicates',[])]
        if not predicates or not all(p in PREDICATES for p in predicates): continue
        if not any('SVE' in p or 'SME' in p for p in predicates): continue
        selected[name]=d
    used=sorted({a[0]['def'] for d in selected.values() for key in ('OutOperandList','InOperandList') for a in d[key]['args']})
    types=[operand_type(n,records) for n in used]; type_ids={t['name']:i for i,t in enumerate(types)}
    forms=[]
    for name,d in selected.items():
        asm=re.sub(r'\{([^{}|]*)\|[^{}]*\}',lambda m:m[1],d['AsmString'])
        asm=asm.replace('\\{','{').replace('\\}','}')
        outs=d['OutOperandList']['args'];ins=d['InOperandList']['args'];params=[]
        ties={b:a for a,b in re.findall(r'\$(\w+)\s*=\s*\$(\w+)',d['Constraints'])}
        all_args=outs+ins
        for index,(ty,var) in enumerate(all_args):
            fields=[]
            for bit,b in enumerate(d['Inst']):
                if isinstance(b,dict) and b['var']==var: fields.append([b.get('index',0),bit])
            params.append(dict(name=var,type=type_ids[ty['def']],tied=next((j for j,(_,v) in enumerate(all_args) if v==ties.get(var)),-1),fields=fields))
        # SME whole-ZA transfers encode one immediate in both printed roles.
        if name in ('LDR_ZA','STR_ZA'):
            next(p for p in params if p['name']=='offset')['tied']=next(j for j,p in enumerate(params) if p['name']=='imm4')
        assert all(isinstance(b,(int,dict)) for b in d['Inst']),name
        base=sum((b << i) for i,b in enumerate(d['Inst']) if isinstance(b,int))
        mask=sum((1 << i) for i,b in enumerate(d['Inst']) if isinstance(b,int))
        for index,p in enumerate(params):
            asm=re.sub(r'\$'+re.escape(p['name'])+r'(?!\w)',lambda m:'$'+str(index),asm)
        assert not re.search(r'\$[A-Za-z_]',asm),(name,asm)
        all_features=[]; any_features=[]
        for pred in (p['def'] for p in d['Predicates']):
            a,b=PREDICATES[pred]
            if a: all_features+=a.split('|')
            if b: any_features+=b.split('|')
        lanes=[types[p['type']]['lane'] for p in params[:len(outs)] if types[p['type']]['lane']]
        input_lanes=[types[p['type']]['lane'] for p in params[len(outs):] if p['tied']<0 and types[p['type']]['lane']]
        literal_lanes=re.findall(r'\$\d+\.(?:\d+)?([bhsdq])',asm)
        bits=next(iter(lanes or input_lanes),0)
        if not bits and literal_lanes: bits={'b':8,'h':16,'s':32,'d':64,'q':128}[literal_lanes[0]]
        in_bits=next((lane for lane in input_lanes if lane!=bits),next(iter(input_lanes),bits))
        forms.append(dict(id=name,name=asm.split()[0],assembly=asm,base=base,mask=mask,
                          parameters=params,results=len(outs),bits=bits,input_bits=in_bits,
                          all=sorted(set(all_features)),any=sorted(set(any_features)),
                          load=bool(d.get('mayLoad')),store=bool(d.get('mayStore')),
                          uses=[r['def'] for r in d.get('Uses',[])],defs=[r['def'] for r in d.get('Defs',[])],
                          predicates=[p['def'] for p in d['Predicates']]))
        forms[-1]['prefix_compatible']=(d.get('DestructiveInstType') or {}).get('def','NotDestructive')!='NotDestructive'
        forms[-1]['prefix_bits']={'ElementSizeB':8,'ElementSizeH':16,'ElementSizeS':32,'ElementSizeD':64}.get((d.get('ElementSize') or {}).get('def'),0)
    return dict(schema=1,upstream='LLVM llvmorg-18.1.8; Apache-2.0 WITH LLVM-exception',
                upstream_url='https://github.com/llvm/llvm-project/tree/llvmorg-18.1.8/llvm/lib/Target/AArch64',
                types=types,forms=sorted(forms,key=lambda f:(f['name'],f['id'])))


def roles(form):
    names={p['name'] for p in form['parameters']}; result=[]
    matrix=any(n.startswith('ZA') for n in names)
    for j,p in enumerate(form['parameters']):
        n=p['name'].lstrip('_'); lower=n.lower()
        if j<form['results']: role='dst' if j==0 else 'dst'+str(j+1)
        elif n=='offset': role='offset'
        elif p['tied']>=0: role='acc' if matrix or ('Zn' in names and 'Zm' in names) else 'lhs'
        elif n=='Pg': role='pred'
        elif n=='Pn': role='rows' if matrix else 'lhs' if 'Pm' in names else 'pred'
        elif n=='Pm': role='cols' if matrix else 'rhs'
        elif n in ('Zn','Zn1'): role='lhs' if 'Zm' in names else 'src'
        elif n in ('Zm','Zm1'): role='rhs'
        elif n in ('Rn','Rn_w'):
            role='addr' if form['load'] or form['store'] else 'lhs' if 'Rm' in names else 'src'
        elif n=='Rm': role='index' if form['load'] or form['store'] else 'rhs'
        elif n in ('Zt','ZAt'): role='dst' if form['load'] else 'src'
        elif n in ('Rv','Ws','Rs'): role='index'
        elif n=='pattern': role='pattern'
        elif lower.startswith('imm') and matrix and (form['load'] or form['store']): role='slice'
        elif lower.startswith('imm'):
            role='offset' if form['load'] or form['store'] else 'rhs' if form['name'] in ('add','sub','subr','and','orr','eor','bic','dup','dupm','cpy','fcpy','fdup') else 'imm'
        elif 'rot' in lower: role='rotation'
        else: role=lower
        if role in result: role=lower
        assert role not in result,(form['id'],role)
        result.append(role)
    return result


def emit_tables(catalog, output):
    types=catalog['types'];forms=catalog['forms']
    # SMSTART/SMSTOP are architectural aliases of the SVCR immediate operation.
    # Keep the semantic mode operand instead of exposing encoded PSTATE bits.
    mode_type=len(types)
    mode=dict(name='streaming_state',kind='MODE',width=0,lane=0,first=0,last=0,group=1,
              extend=0,shift=0,low=1,high=3,scale=1,transform=0,printer='')
    types=types+[mode]
    controls=[]
    for start in (False,True):
        controls.append(dict(id='SMSTART' if start else 'SMSTOP',name='smstart' if start else 'smstop',
            assembly=('smstart' if start else 'smstop')+'\t$0',base=0xd503417f if start else 0xd503407f,
            mask=0xfffff9ff,parameters=[dict(name='state',type=mode_type,tied=-1,fields=[[0,9],[1,10]])],
            results=0,bits=0,input_bits=0,all=['SME'],any=[],load=False,store=False,uses=[],defs=[],predicates=['HasSME']))
    forms=sorted(forms+controls,key=lambda f:(f['name'],f['id']))
    lines=['/* Generated by tools/generate_scalable_isa.py; do not edit. */',
           '/* Encoding facts normalized from LLVM llvmorg-18.1.8. See tools/isa/aarch64_scalable.json. */',
           'const A64ScalableType a64_scalable_types[] = {']
    for t in types:
        lines.append('    {'+','.join([json.dumps(t['name']),'A64_SC_'+t['kind']]+[str(t[k]) for k in
            ('width','lane','first','last','group','extend','shift','low','high','scale','transform')])+ '},')
    lines+=['};','const A64ScalableParameter a64_scalable_parameters[] = {']
    offset=0;field_offset=0;field_rows=[];form_rows=[]
    def features(values):return ' | '.join('AST_TARGET_FEATURE_A64_'+v for v in values) or '0'
    for f in forms:
        names=roles(f)
        for p,role in zip(f['parameters'],names):
            if role=='state':default=3
            elif role=='pattern':default=31
            elif types[p['type']]['kind']=='INC':default=1
            else:default=-1
            lines.append('    {'+','.join([json.dumps(role),str(p['type']),str(p['tied']),str(default)])+'},')
        for p_index,p in enumerate(f['parameters']):
            fields=p['fields'];start=0
            while start<len(fields):
                end=start+1
                while end<len(fields) and fields[end]==[fields[start][0]+end-start,fields[start][1]+end-start]:end+=1
                field_rows.append('    {'+f'{p_index},{fields[start][0]},{fields[start][1]},{end-start}'+'},')
                start=end
        count=len(field_rows)-field_offset
        matrix=any(types[p['type']]['kind'].startswith('ZA') for p in f['parameters']) or f['name']=='zero'
        flags=(1 if f['load'] else 0)|(2 if f['store'] else 0)|(4 if matrix else 0)
        if matrix and any(types[p['type']]['kind']=='Z' for p in f['parameters']):flags|=8
        if f['name']=='smstart':flags|=16
        if f['name']=='smstop':flags|=32
        if f['name']=='movprfx':flags|=64
        if any(p['tied']>=0 for p in f['parameters']):flags|=128
        if 'NZCV' in f['defs']:flags|=256
        if f.get('prefix_compatible'):flags|=1024
        if '/m' in f['assembly']:flags|=2048
        if 'HasSVEorSME' not in f['predicates'] and 'HasSVE2orSME' not in f['predicates'] and not any('SME' in p for p in f['predicates']):flags|=512
        extends={types[p['type']]['extend'] for p in f['parameters'] if types[p['type']]['extend']}
        shifts={types[p['type']]['shift'] for p in f['parameters'] if types[p['type']]['shift']}
        assert len(extends)<=1 and len(shifts)<=1,f['id']
        form_rows.append('    {'+','.join([json.dumps(f['name']),json.dumps(f['assembly']),features(f['all']),features(f['any']),
            f'UINT32_C(0x{f["base"]:08x})',f'UINT32_C(0x{f["mask"]:08x})',str(offset),str(field_offset),str(count),
            str(len(f['parameters'])),str(f['results']),str(f['bits']),str(f['input_bits']),str(flags),
            str(next(iter(extends),0)),str(next(iter(shifts),0)),str(f.get('prefix_bits',0))])+'}, /* '+f['id']+' */')
        offset+=len(f['parameters']);field_offset+=count
    lines+=['};','const A64ScalableField a64_scalable_fields[] = {']+field_rows+['};',
            'const A64ScalableForm a64_scalable_forms[] = {']+form_rows+['};',
            f'const uint32_t a64_scalable_form_count = {len(forms)}u;',
            f'const uint32_t a64_scalable_type_count = {len(types)}u;','']
    output.write_text('\n'.join(lines),encoding='utf-8')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--records',type=Path)
    parser.add_argument('--catalog',type=Path,default=ROOT/'tools/isa/aarch64_scalable.json')
    parser.add_argument('--output',type=Path,default=ROOT/'src/isa/aarch64_scalable_tables.inc')
    parser.add_argument('--clang-format',default=shutil.which('clang-format'))
    args=parser.parse_args()
    if not args.clang_format: parser.error('clang-format is required to reproduce the LLVM-style C table')
    if args.records:
        catalog=normalize(json.loads(args.records.read_text(encoding='utf-8')))
        args.catalog.parent.mkdir(parents=True,exist_ok=True)
        args.catalog.write_text(json.dumps(catalog,indent=2)+'\n',encoding='utf-8')
    else: catalog=json.loads(args.catalog.read_text(encoding='utf-8'))
    emit_tables(catalog,args.output)
    subprocess.run([args.clang_format,'-i','--style=file',str(args.output)],check=True)
    print(len(catalog['forms']),'scalable forms;',len(catalog['types']),'operand types;',
          len({f['name'] for f in catalog['forms']}),'mnemonics')


if __name__=='__main__':main()
