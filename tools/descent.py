import sys, os, struct, hashlib, collections, json
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m68k
from m68k import Illegal

ROM = open(os.environ.get('S3D_ROM', 'rom.bin'), 'rb').read()
ROMEND = len(ROM)

# seeds that no instruction in ROM references (RAM-code images, unrolled runs)
LINEAR = [(0x1384, 0x178C), (0x1BF0, 0x1CF2)]

MANUAL = {
    0x721C: 'ram code DMA entry 0', 0x7250: 'ram code DMA entry 1', 0x725C: 'ram code DMA entry 2',
    0x5E96: 'ram code HBI long', 0x5ED8: 'ram code HBI short',
    0x1384: 'unrolled copy run', 0x1588: 'unrolled fill run', 0x1BF2: 'unrolled long copy run',
}


def u32(a):
    return struct.unpack('>I', ROM[a:a + 4])[0]


def bra_run(base):
    """Return (start, [targets]) of the bra.w run containing base."""
    s = base
    while s >= 4 and ROM[s - 4] == 0x60 and ROM[s - 3] == 0:
        s -= 4
    e = s
    tg = []
    while e + 4 <= ROMEND and ROM[e] == 0x60 and ROM[e + 1] == 0:
        d = (ROM[e + 2] << 8) | ROM[e + 3]
        d = d - 0x10000 if d & 0x8000 else d
        tg.append((e + 2 + d) & 0xFFFFFFFF)
        e += 4
    return s, tg


def descend(extra=()):
    insns = {}
    work = []
    why = {}
    unresolved = []
    tables = {}
    illegal = []
    ptrvar = {}

    def add(a, reason):
        if a & 1 or a < 0x200 and a != 0 or a >= ROMEND:
            return
        if a not in why:
            why[a] = reason
        work.append(a)

    # vectors
    for v in range(1, 64):
        t = u32(v * 4)
        if 0x200 <= t < ROMEND and not t & 1:
            add(t, 'vector %d' % v)
    add(u32(4), 'reset')
    for a, w in MANUAL.items():
        add(a, w)
    for a in extra:
        add(a, 'extra')
    while work:
        a = work.pop()
        while True:
            if a in insns:
                break
            if a >= ROMEND or a < 0x200 or a & 1:
                break
            try:
                i = m68k.decode(ROM, a)
            except Illegal as e:
                illegal.append((a, str(e)))
                break
            insns[a] = i
            k = i.kind
            nxt = a + i.size
            # stores of code pointers into $FF0402
            if i.mnem == 'move' and i.sz == 'l' and len(i.ops) == 2 and i.ops[0][0] == 'imm' \
                    and i.ops[1][0] in ('absl', 'absw') and (i.ops[1][1] & 0xFFFF) == 0x402 \
                    and i.ops[1][1] >> 16 in (0xFF, 0xFFFF):
                t = i.ops[0][1]
                if 0x200 <= t < ROMEND and not t & 1:
                    ptrvar[t] = a
                    add(t, 'VBIRoutine store @%X' % a)
            if k in ('bra', 'bsr', 'bcc', 'dbcc'):
                add(i.target, 'branch from %X' % a)
                if k == 'bra':
                    break
            elif k == 'jsr' or k == 'jmp':
                if i.target is not None:
                    add(i.target, 'call from %X' % a)
                else:
                    o = i.ops[0]
                    if o[0] == 'pci':
                        s, tg = bra_run(o[1])
                        if tg:
                            tables[a] = (s, tg)
                            for t in tg:
                                add(t, 'table %X' % a)
                        else:
                            unresolved.append((a, 'pci no bra table', o))
                    else:
                        unresolved.append((a, 'indirect', o))
                if k == 'jmp':
                    break
            elif k in ('rts', 'rte', 'rtr', 'stop'):
                break
            a = nxt
    # table entries are instructions too (bra.w); they are the targets of the dispatching jmp/jsr
    for site, (s0, tg) in tables.items():
        for k in range(len(tg)):
            a0 = s0 + 4 * k
            insns.setdefault(a0, m68k.decode(ROM, a0))
    # computed entries into straight-line runs: every word is a possible entry
    for lo, hi in LINEAR:
        a = lo
        while a < hi:
            i = m68k.decode(ROM, a)
            insns.setdefault(a, i)
            a += i.size
    return insns, why, unresolved, tables, illegal, ptrvar


def block_hashes(insns):
    blocks = collections.defaultdict(list)
    for a in sorted(insns):
        blocks[a >> 12].append('%x:%d' % (a, insns[a].size))
    return {hex(k << 12)[2:]: hashlib.md5(','.join(v).encode()).hexdigest()[:6] for k, v in sorted(blocks.items())}


if __name__ == '__main__':
    insns, why, unresolved, tables, illegal, ptrvar = descend()
    print('insns', len(insns), 'bytes', sum(i.size for i in insns.values()))
    print('illegal stops', len(illegal), illegal[:10])
    print('unresolved', [(hex(a), r, o) for a, r, o in unresolved])
    print('tables', {hex(k): (hex(v[0]), len(v[1])) for k, v in tables.items()})
    print('vbi targets', sorted(hex(t) for t in ptrvar))
    json.dump(block_hashes(insns), open('my_bh.json', 'w'))
