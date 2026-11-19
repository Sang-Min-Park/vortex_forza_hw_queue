#!/usr/bin/env python3
"""Generate self-checking ASM using independent Python reference results."""
import argparse, hashlib, json, os, random, resource, subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parent
OPS = 'andn bclr bclri bext bexti binv binvi bset bseti clz cpop ctz max maxu min minu orc.b orn rev8 rol ror rori sh1add sh2add sh3add xnor'.split()
UNARY = {'clz', 'cpop', 'ctz', 'orc.b', 'rev8'}
IMMEDIATE = {'bclri', 'bexti', 'binvi', 'bseti', 'rori'}

def reference(op, a, b, w):
    mask = (1 << w) - 1
    a, b = a & mask, b & mask
    n = b % w
    signed = lambda v: v if v < 1 << (w - 1) else v - (1 << w)
    if op == 'andn': r = a & ~b
    elif op.startswith('bclr'): r = a & ~(1 << n)
    elif op.startswith('bext'): r = (a >> n) & 1
    elif op.startswith('binv'): r = a ^ (1 << n)
    elif op.startswith('bset'): r = a | (1 << n)
    elif op == 'clz': r = w - a.bit_length()
    elif op == 'ctz': r = (a & -a).bit_length() - 1 if a else w
    elif op == 'cpop': r = a.bit_count()
    elif op == 'max': r = max(signed(a), signed(b))
    elif op == 'min': r = min(signed(a), signed(b))
    elif op == 'maxu': r = max(a, b)
    elif op == 'minu': r = min(a, b)
    elif op == 'orc.b': r = sum(255 << (8*i) for i in range(w//8) if (a >> (8*i)) & 255)
    elif op == 'orn': r = a | ~b
    elif op == 'rev8': r = int.from_bytes(a.to_bytes(w//8, 'little'), 'big')
    elif op == 'rol': r = (a << n) | (a >> ((w-n) % w))
    elif op in {'ror', 'rori'}: r = (a >> n) | (a << ((w-n) % w))
    elif op.startswith('sh'): r = (a << int(op[2])) + b
    elif op == 'xnor': r = ~(a ^ b)
    else: raise ValueError(op)
    return r & mask

def load_constant(reg, value):
    # Keep LI expansion in the base ISA; Zba can otherwise introduce SLLI.UW.
    return f'.option push\n.option arch, -zba,-zbb,-zbs\n    li {reg}, 0x{value:x}\n.option pop'

def source(op, w, negative):
    mask = (1 << w) - 1
    rng = random.Random(3100)
    values = [0, 1, mask, 1 << (w-1), (1 << (w-1))-1, 0x80000000, 0x0123456789abcdef & mask, 0xff000100]
    pairs = [(a,b) for a in values for b in [0,1,31,32,w-1,w,w+1,mask]]
    pairs += [(rng.getrandbits(w), rng.getrandbits(w)) for _ in range(24)]
    if op in UNARY:
        pairs += [(1 << bit, 0) for bit in range(w)]
    if op in IMMEDIATE or op in {'bclr', 'bext', 'binv', 'bset', 'rol', 'ror'}:
        pairs += [(0x89abcdef01234567 & mask, bit) for bit in range(w)]
    lines = ['.option norvc', '.option norelax', '.text', '.globl _start', '_start:']
    count = 0
    for index,(a,b) in enumerate(pairs):
        if op in IMMEDIATE: b %= w
        layouts = [('t2','t0','t1')]
        if index % 8 == 0:
            layouts += [('t0','t0','t1'), ('t1','t0','t1'), ('zero','t0','t1')]
            if op not in UNARY | IMMEDIATE:
                layouts += [('t2','t0','t0'), ('t2','zero','t1'), ('t2','t0','zero')]
        for rd,rs1,rs2 in layouts:
            count += 1
            aa = 0 if rs1 == 'zero' else a
            bb = a if rs2 == 't0' else 0 if rs2 == 'zero' else b
            expected = reference(op, aa, b if op in IMMEDIATE else bb, w)
            if rd == 'zero': expected = 0
            if negative and count == 1: expected ^= 1
            args = f'{rd}, {rs1}'
            if op in IMMEDIATE: args += f', {b}'
            elif op not in UNARY: args += f', {rs2}'
            lines += [f'    li gp, {count}', load_constant('t0', a), load_constant('t1', b), f'    {op} {args}', load_constant('t3', expected), f'    bne {rd}, t3, report']
    lines += ['    li gp, 0', 'report:', '    la t0, tohost', '    slli gp, gp, 1', '    ori gp, gp, 1', '    sw gp, 0(t0)', '    fence iorw, iorw', 'halt: j halt', '.section .tohost,"aw",@progbits', '.balign 64', '.globl tohost', 'tohost: .dword 0']
    return '\n'.join(lines)+'\n', count

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--xlen', type=int, choices=[32,64], required=True)
    p.add_argument('--sim', type=Path, required=True)
    p.add_argument('--output', type=Path, default=Path('results'))
    p.add_argument('--clang', type=Path, required=True)
    p.add_argument('--ops', nargs='+', choices=OPS, default=OPS)
    p.add_argument('--negative', action='store_true')
    args = p.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0,0))
    sim = args.sim.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    clang = args.clang.resolve()
    env = {k:v for k,v in os.environ.items() if k not in {'DEBUG','CONFIGS','VORTEX_HOME','XLEN'}}
    rows = []
    for op in args.ops:
        for negative in ([False,True] if args.negative else [False]):
            name = op.replace('.','_') + ('_negative' if negative else '')
            asm, count = source(op, args.xlen, negative)
            src, elf = out/(name+'.S'), out/(name+'.elf')
            src.write_text(asm)
            cmd = [str(clang),f'--target=riscv{args.xlen}-unknown-elf',f'-march=rv{args.xlen}im_zicsr_zba_zbb_zbs','-mabi='+('lp64' if args.xlen==64 else 'ilp32'),'-mcmodel=medany','-mno-relax','-nostdlib','-fuse-ld=lld','-Wl,--no-relax','-Wl,-T,'+str(ROOT/'link.ld'),str(src),'-o',str(elf)]
            build = subprocess.run(cmd, capture_output=True, text=True, env=env)
            (out/(name+'.build.log')).write_text(build.stdout+build.stderr)
            if build.returncode: raise SystemExit('Build failed: '+name)
            dis = subprocess.run([str(clang.with_name('llvm-objdump')),'-d','--mattr=+zba,+zbb,+zbs',str(elf)], capture_output=True, text=True, check=True)
            (out/(name+'.dis')).write_text(dis.stdout)
            try:
                run = subprocess.run([str(sim),str(elf)],capture_output=True,text=True,env=env,timeout=30)
                rc,log = run.returncode,run.stdout+run.stderr
            except subprocess.TimeoutExpired: rc,log = 'TIMEOUT','TIMEOUT'
            (out/(name+'.run.log')).write_text(log)
            expected = 1 if negative else 0
            rows.append(dict(test=name,vectors=count,expected=expected,actual=rc,passed=rc==expected))
            print(f'{name:18} exit={str(rc):5} expected={expected} {"PASS" if rc==expected else "FAIL"}',flush=True)
    (out/'results.json').write_text(json.dumps(dict(xlen=args.xlen,simulator=str(sim),simulator_sha256=hashlib.sha256(sim.read_bytes()).hexdigest(),tests=rows),indent=2)+'\n')
    return 0 if all(r['passed'] for r in rows) else 1
if __name__ == '__main__': raise SystemExit(main())
