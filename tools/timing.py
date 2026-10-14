"""Approximate MC68000 instruction timing (cycles), after the M68000 user manual tables."""
import sys, os
sys.dont_write_bytecode = True

def ea_c(o, sz):
    k = o[0]
    long_ = (sz == 'l')
    if k in ('d', 'a'): return 0
    t = {'ind': (4, 8), 'post': (4, 8), 'pre': (6, 10), 'disp': (8, 12), 'idx': (10, 14),
         'absw': (8, 12), 'absl': (12, 16), 'pcd': (8, 12), 'pci': (10, 14), 'imm': (4, 8)}[k]
    return t[1] if long_ else t[0]

def is_reg(o): return o[0] in ('d', 'a', 'imm')

LEA = {'ind': 4, 'disp': 8, 'idx': 12, 'absw': 8, 'absl': 12, 'pcd': 8, 'pci': 12}
PEA = {'ind': 12, 'disp': 16, 'idx': 20, 'absw': 16, 'absl': 20, 'pcd': 16, 'pci': 20}
JSR = {'ind': 16, 'disp': 18, 'idx': 22, 'absw': 18, 'absl': 20, 'pcd': 18, 'pci': 22}
JMP = {'ind': 8, 'disp': 10, 'idx': 14, 'absw': 10, 'absl': 12, 'pcd': 10, 'pci': 14}
MOVEM_ST = {'ind': 8, 'pre': 8, 'disp': 12, 'idx': 14, 'absw': 12, 'absl': 16}
MOVEM_LD = {'ind': 12, 'post': 12, 'disp': 16, 'idx': 18, 'absw': 16, 'absl': 20, 'pcd': 16, 'pci': 18}

def branch_taken(i): return 10
def branch_nottaken(i): return 8 if i.sz == 's' else 12

def cycles(i):
    m, sz, ops = i.mnem, i.sz, i.ops
    L = sz == 'l'
    if m == 'nop': return 4
    if m == 'moveq': return 4
    if m in ('move', 'movea'):
        s, d = ops
        if d[0] == 'sr' or d[0] == 'ccr': return 12 + ea_c(s, 'w')
        if s[0] == 'sr': return 6 if d[0] == 'd' else 8 + ea_c(d, 'w')
        if d[0] == 'usp' or s[0] == 'usp': return 4
        return 4 + ea_c(s, sz) + ea_c(d, sz)
    if m == 'lea': return LEA[ops[0][0]]
    if m == 'pea': return PEA[ops[0][0]]
    if m == 'clr':
        return (6 if L else 4) if ops[0][0] == 'd' else (12 if L else 8) + ea_c(ops[0], sz)
    if m in ('add', 'sub', 'and', 'or'):
        s, d = ops
        if d[0] == 'd':
            if L: return 8 if is_reg(s) else 6 + ea_c(s, sz)
            return 4 + ea_c(s, sz)
        return (12 if L else 8) + ea_c(d, sz)
    if m == 'eor':
        d = ops[1]
        if d[0] == 'd': return 8 if L else 4
        return (12 if L else 8) + ea_c(d, sz)
    if m in ('addi', 'subi', 'andi', 'ori', 'eori'):
        d = ops[1]
        if d[0] in ('ccr', 'sr'): return 20
        if d[0] == 'd': return 16 if L else 8
        return (20 if L else 12) + ea_c(d, sz)
    if m == 'cmpi':
        d = ops[1]
        if d[0] == 'd': return 14 if L else 8
        return (12 if L else 8) + ea_c(d, sz)
    if m in ('addq', 'subq'):
        d = ops[1]
        if d[0] == 'a': return 8
        if d[0] == 'd': return 8 if L else 4
        return (12 if L else 8) + ea_c(d, sz)
    if m in ('adda', 'suba'):
        s = ops[0]
        if L: return 8 if is_reg(s) else 6 + ea_c(s, sz)
        return 8 + ea_c(s, sz)
    if m == 'cmpa': return 6 + ea_c(ops[0], sz)
    if m == 'cmp': return (6 if L else 4) + ea_c(ops[0], sz)
    if m == 'cmpm': return 20 if L else 12
    if m in ('addx', 'subx'):
        if ops[0][0] == 'd': return 8 if L else 4
        return 30 if L else 18
    if m in ('abcd', 'sbcd'): return 6 if ops[0][0] == 'd' else 18
    if m in ('neg', 'negx', 'not'):
        d = ops[0]
        if d[0] == 'd': return 6 if L else 4
        return (12 if L else 8) + ea_c(d, sz)
    if m == 'nbcd': return 6 if ops[0][0] == 'd' else 8 + ea_c(ops[0], 'b')
    if m == 'tst': return 4 + ea_c(ops[0], sz)
    if m in ('ext', 'swap'): return 4
    if m == 'exg': return 6
    if m == 's': return 6 if ops[0][0] == 'd' else 8 + ea_c(ops[0], 'b')
    if m in ('asl', 'asr', 'lsl', 'lsr', 'rol', 'ror', 'roxl', 'roxr'):
        if len(ops) == 1: return 8 + ea_c(ops[0], 'w')
        n = ops[0][1] if ops[0][0] == 'imm' else 4
        return (8 if L else 6) + 2 * n
    if m in ('mulu', 'muls'): return 54 + ea_c(ops[0], 'w')
    if m == 'divu': return 100 + ea_c(ops[0], 'w')
    if m == 'divs': return 140 + ea_c(ops[0], 'w')
    if m in ('btst', 'bchg', 'bclr', 'bset'):
        src, dst = ops
        static = src[0] == 'imm'
        if dst[0] == 'd':
            if m == 'btst': return 10 if static else 6
            if m == 'bclr': return 14 if static else 10
            return 12 if static else 8
        base = {'btst': 4, 'bchg': 8, 'bclr': 8, 'bset': 8}[m] + (4 if static else 0)
        return base + ea_c(dst, 'b')
    if m == 'movem':
        n = bin(ops[0][1] if ops[0][0] == 'list' else ops[1][1]).count('1')
        per = 8 if L else 4
        if ops[0][0] == 'list': return MOVEM_ST[ops[1][0]] + per * n
        return MOVEM_LD[ops[0][0]] + per * n
    if m == 'movep': return 24 if L else 16
    if m == 'bra': return 10
    if m == 'b': return 0
    if m == 'db': return 0
    if m == 'bsr': return 18
    if m == 'jsr': return JSR[ops[0][0]]
    if m == 'jmp': return JMP[ops[0][0]]
    if m == 'rts': return 16
    if m in ('rte', 'rtr'): return 20
    return 8
