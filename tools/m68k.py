"""Motorola 68000 instruction decoder (full user+supervisor set of the 68000).

decode(rom, pc) -> Insn.  Raises Illegal for anything that is not a valid 68000
encoding.  Operand forms (tuples):
  ('d', n)  ('a', n)  ('ind', n)  ('post', n)  ('pre', n)
  ('disp', n, d16)  ('idx', n, d8, xreg, xis_a, xlong)
  ('absw', addr32)  ('absl', addr32)
  ('pcd', target)  ('pci', base, xreg, xis_a, xlong)   # base = pc + d8
  ('imm', value)  ('sr',) ('ccr',) ('usp',) ('list', mask, reversed)
"""
import struct


class Illegal(Exception):
    pass


CC = ['t', 'f', 'hi', 'ls', 'cc', 'cs', 'ne', 'eq', 'vc', 'vs', 'pl', 'mi', 'ge', 'lt', 'gt', 'le']
SZ = {0: 'b', 1: 'w', 2: 'l'}


class Insn:
    __slots__ = ('addr', 'size', 'mnem', 'sz', 'ops', 'cc', 'kind', 'target', 'words')

    def __init__(self, addr):
        self.addr = addr
        self.size = 2
        self.mnem = '?'
        self.sz = None
        self.ops = []
        self.cc = None
        self.kind = 'seq'
        self.target = None
        self.words = 1

    def __repr__(self):
        return '%06X %s' % (self.addr, fmt(self))


def s8(v):
    return v - 0x100 if v & 0x80 else v


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


class Reader:
    def __init__(self, rom, pc):
        self.rom = rom
        self.pc = pc
        self.n = 0

    def w(self):
        a = self.pc + self.n
        if a + 2 > len(self.rom) or a < 0:
            raise Illegal('oob')
        v = (self.rom[a] << 8) | self.rom[a + 1]
        self.n += 2
        return v

    def l(self):
        h = self.w()
        return (h << 16) | self.w()


def ea_op(r, mode, reg, size):
    """Decode an effective address; may consume extension words."""
    if mode == 0:
        return ('d', reg)
    if mode == 1:
        return ('a', reg)
    if mode == 2:
        return ('ind', reg)
    if mode == 3:
        return ('post', reg)
    if mode == 4:
        return ('pre', reg)
    if mode == 5:
        return ('disp', reg, s16(r.w()))
    if mode == 6:
        e = r.w()
        if e & 0x0700:
            raise Illegal('brief ext')
        return ('idx', reg, s8(e & 0xFF), (e >> 12) & 7, bool(e & 0x8000), bool(e & 0x0800))
    if mode == 7:
        if reg == 0:
            return ('absw', s16(r.w()) & 0xFFFFFFFF)
        if reg == 1:
            return ('absl', r.l())
        if reg == 2:
            base = r.pc + r.n
            return ('pcd', (base + s16(r.w())) & 0xFFFFFFFF)
        if reg == 3:
            base = r.pc + r.n
            e = r.w()
            if e & 0x0700:
                raise Illegal('brief ext')
            return ('pci', (base + s8(e & 0xFF)) & 0xFFFFFFFF, (e >> 12) & 7, bool(e & 0x8000), bool(e & 0x0800))
        if reg == 4:
            if size == 'b':
                return ('imm', r.w() & 0xFF)
            if size == 'w':
                return ('imm', r.w())
            return ('imm', r.l())
    raise Illegal('ea')


def is_data_alterable(op):
    return op[0] in ('d', 'ind', 'post', 'pre', 'disp', 'idx', 'absw', 'absl')


def is_memory_alterable(op):
    return op[0] in ('ind', 'post', 'pre', 'disp', 'idx', 'absw', 'absl')


def is_control(op):
    return op[0] in ('ind', 'disp', 'idx', 'absw', 'absl', 'pcd', 'pci')


def decode(rom, pc):
    if pc & 1:
        raise Illegal('odd pc')
    r = Reader(rom, pc)
    op = r.w()
    i = Insn(pc)
    line = op >> 12
    mode = (op >> 3) & 7
    reg = op & 7
    dn = (op >> 9) & 7

    if line == 0:
        _line0(r, i, op, mode, reg, dn)
    elif line in (1, 2, 3):
        sz = {1: 'b', 3: 'w', 2: 'l'}[line]
        src = ea_op(r, mode, reg, sz)
        dmode = (op >> 6) & 7
        dst = ea_op(r, dmode, dn, sz)
        if dmode == 1:
            if sz == 'b':
                raise Illegal('movea.b')
            i.mnem = 'movea'
        else:
            i.mnem = 'move'
            if not is_data_alterable(dst):
                raise Illegal('move dst')
        if sz == 'b' and src[0] == 'a':
            raise Illegal('move.b An')
        i.sz = sz
        i.ops = [src, dst]
    elif line == 4:
        _line4(r, i, op, mode, reg, dn)
    elif line == 5:
        _line5(r, i, op, mode, reg, dn)
    elif line == 6:
        cond = (op >> 8) & 0xF
        d = op & 0xFF
        base = pc + 2
        if d == 0:
            d = s16(r.w())
        elif d == 0xFF:
            raise Illegal('long branch')
        else:
            d = s8(d)
        i.target = (base + d) & 0xFFFFFFFF
        if cond == 0:
            i.mnem, i.kind = 'bra', 'bra'
        elif cond == 1:
            i.mnem, i.kind = 'bsr', 'bsr'
        else:
            i.mnem, i.kind, i.cc = 'b', 'bcc', cond
        i.ops = [('pcd', i.target)]
        i.sz = 's' if (op & 0xFF) else 'w'
    elif line == 7:
        if op & 0x100:
            raise Illegal('moveq')
        i.mnem, i.sz = 'moveq', 'l'
        i.ops = [('imm', s8(op & 0xFF) & 0xFFFFFFFF), ('d', dn)]
    elif line == 8:
        _line8(r, i, op, mode, reg, dn)
    elif line in (9, 0xD):
        _addsub(r, i, op, mode, reg, dn, 'sub' if line == 9 else 'add')
    elif line == 0xB:
        _lineb(r, i, op, mode, reg, dn)
    elif line == 0xC:
        _linec(r, i, op, mode, reg, dn)
    elif line == 0xE:
        _linee(r, i, op, mode, reg, dn)
    else:
        raise Illegal('line a/f')
    i.size = r.n if r.n else 2
    # r.n counts words consumed after the opcode word only through r.w(); opcode itself read via r.w too
    i.size = r.n
    return i


def _line0(r, i, op, mode, reg, dn):
    if op & 0x100:
        opm = (op >> 6) & 7
        if mode == 1:      # movep
            if opm < 4:
                raise Illegal('movep')
            d = s16(r.w())
            i.mnem = 'movep'
            i.sz = 'w' if opm in (4, 6) else 'l'
            mem = ('disp', reg, d)
            i.ops = [mem, ('d', dn)] if opm in (4, 5) else [('d', dn), mem]
            return
        names = ['btst', 'bchg', 'bclr', 'bset']
        i.mnem = names[(op >> 6) & 3]
        dst = ea_op(r, mode, reg, 'b')
        if i.mnem == 'btst':
            if dst[0] == 'a' or (dst[0] == 'imm' and False):
                raise Illegal('btst An')
        elif not is_data_alterable(dst):
            raise Illegal('bit dst')
        i.sz = 'l' if dst[0] == 'd' else 'b'
        i.ops = [('d', dn), dst]
        return
    k = (op >> 9) & 7
    szf = (op >> 6) & 3
    if k == 4:
        names = ['btst', 'bchg', 'bclr', 'bset']
        i.mnem = names[szf]
        bit = r.w() & 0xFF
        dst = ea_op(r, mode, reg, 'b')
        if i.mnem == 'btst':
            if dst[0] in ('a', 'imm'):
                raise Illegal('btst')
        elif not is_data_alterable(dst):
            raise Illegal('bit dst')
        i.sz = 'l' if dst[0] == 'd' else 'b'
        i.ops = [('imm', bit), dst]
        return
    names = {0: 'ori', 1: 'andi', 2: 'subi', 3: 'addi', 5: 'eori', 6: 'cmpi'}
    if k not in names or szf == 3:
        raise Illegal('line0')
    sz = SZ[szf]
    i.mnem = names[k]
    i.sz = sz
    if k in (0, 1, 5) and mode == 7 and reg == 4:
        if sz == 'b':
            v = r.w() & 0xFF
            i.ops = [('imm', v), ('ccr',)]
            return
        if sz == 'w':
            v = r.w()
            i.ops = [('imm', v), ('sr',)]
            return
        raise Illegal('imm sr')
    if sz == 'b':
        v = r.w() & 0xFF
    elif sz == 'w':
        v = r.w()
    else:
        v = r.l()
    dst = ea_op(r, mode, reg, sz)
    if k == 6:
        if dst[0] in ('a', 'imm'):
            raise Illegal('cmpi')
    elif not is_data_alterable(dst):
        raise Illegal('imm dst')
    i.ops = [('imm', v), dst]


def _line4(r, i, op, mode, reg, dn):
    if (op & 0x1C0) == 0x1C0:
        dst_an = ('a', dn)
        src = ea_op(r, mode, reg, 'l')
        if not is_control(src):
            raise Illegal('lea')
        i.mnem, i.sz, i.ops = 'lea', 'l', [src, dst_an]
        return
    if (op & 0x1C0) == 0x180:
        src = ea_op(r, mode, reg, 'w')
        if src[0] == 'a':
            raise Illegal('chk')
        i.mnem, i.sz, i.ops = 'chk', 'w', [src, ('d', dn)]
        return
    if (op & 0xFFF8) == 0x4E50:
        d = s16(r.w())
        i.mnem, i.ops = 'link', [('a', reg), ('imm', d & 0xFFFF)]
        return
    if (op & 0xFFF8) == 0x4E58:
        i.mnem, i.ops = 'unlk', [('a', reg)]
        return
    if (op & 0xFFF0) == 0x4E40:
        i.mnem, i.ops, i.kind = 'trap', [('imm', op & 0xF)], 'trap'
        return
    if (op & 0xFFF8) == 0x4E60:
        i.mnem, i.sz, i.ops = 'move', 'l', [('a', reg), ('usp',)]
        return
    if (op & 0xFFF8) == 0x4E68:
        i.mnem, i.sz, i.ops = 'move', 'l', [('usp',), ('a', reg)]
        return
    fixed = {0x4E70: ('reset', 'seq'), 0x4E71: ('nop', 'seq'), 0x4E73: ('rte', 'rte'),
             0x4E75: ('rts', 'rts'), 0x4E76: ('trapv', 'seq'), 0x4E77: ('rtr', 'rtr')}
    if op in fixed:
        i.mnem, i.kind = fixed[op]
        return
    if op == 0x4E72:
        i.mnem, i.ops, i.kind = 'stop', [('imm', r.w())], 'stop'
        return
    if op == 0x4AFC:
        raise Illegal('illegal')
    if (op & 0xFFC0) == 0x4E80 or (op & 0xFFC0) == 0x4EC0:
        src = ea_op(r, mode, reg, 'l')
        if not is_control(src):
            raise Illegal('jmp/jsr')
        if (op & 0x40):
            i.mnem, i.kind = 'jmp', 'jmp'
        else:
            i.mnem, i.kind = 'jsr', 'jsr'
        i.ops = [src]
        if src[0] in ('absw', 'absl', 'pcd'):
            i.target = src[1] & 0xFFFFFFFF
        return
    # movem
    if (op & 0xFB80) == 0x4880 and mode != 0:
        sz = 'l' if op & 0x40 else 'w'
        mask = r.w()
        ea = ea_op(r, mode, reg, sz)
        to_reg = bool(op & 0x400)
        if to_reg:
            if ea[0] in ('d', 'a', 'pre', 'imm'):
                raise Illegal('movem')
        else:
            if ea[0] in ('d', 'a', 'post', 'imm', 'pcd', 'pci'):
                raise Illegal('movem')
        i.mnem, i.sz = 'movem', sz
        lst = ('list', mask, ea[0] == 'pre')
        i.ops = [ea, lst] if to_reg else [lst, ea]
        return
    if (op & 0xFFF8) == 0x4840:
        i.mnem, i.sz, i.ops = 'swap', 'w', [('d', reg)]
        return
    if (op & 0xFFC0) == 0x4840:
        src = ea_op(r, mode, reg, 'l')
        if not is_control(src):
            raise Illegal('pea')
        i.mnem, i.sz, i.ops = 'pea', 'l', [src]
        return
    if (op & 0xFFF8) in (0x4880, 0x48C0):
        i.mnem, i.sz, i.ops = 'ext', ('w' if (op & 0x40) == 0 else 'l'), [('d', reg)]
        return
    if (op & 0xFFC0) == 0x4800:
        dst = ea_op(r, mode, reg, 'b')
        if not is_data_alterable(dst):
            raise Illegal('nbcd')
        i.mnem, i.sz, i.ops = 'nbcd', 'b', [dst]
        return
    if (op & 0xFFC0) == 0x4AC0:
        dst = ea_op(r, mode, reg, 'b')
        if not is_data_alterable(dst):
            raise Illegal('tas')
        i.mnem, i.sz, i.ops = 'tas', 'b', [dst]
        return
    grp = (op >> 9) & 7
    szf = (op >> 6) & 3
    if grp in (0, 1, 2, 3) and szf == 3:
        if grp == 1:
            raise Illegal('move from ccr')
        dst_or_src = ea_op(r, mode, reg, 'w')
        if grp == 0:
            if not is_data_alterable(dst_or_src):
                raise Illegal('move sr')
            i.mnem, i.sz, i.ops = 'move', 'w', [('sr',), dst_or_src]
        else:
            if dst_or_src[0] == 'a':
                raise Illegal('move to sr')
            i.mnem, i.sz = 'move', 'w'
            i.ops = [dst_or_src, ('ccr',) if grp == 2 else ('sr',)]
        return
    names = {0: 'negx', 1: 'clr', 2: 'neg', 3: 'not', 5: 'tst'}
    if grp in names and szf != 3:
        sz = SZ[szf]
        dst = ea_op(r, mode, reg, sz)
        if grp == 5:
            if dst[0] in ('imm',) or (dst[0] == 'a'):
                raise Illegal('tst')
        elif not is_data_alterable(dst):
            raise Illegal('unary dst')
        i.mnem, i.sz, i.ops = names[grp], sz, [dst]
        return
    raise Illegal('line4')


def _line5(r, i, op, mode, reg, dn):
    szf = (op >> 6) & 3
    if szf == 3:
        cond = (op >> 8) & 0xF
        if mode == 1:
            d = s16(r.w())
            i.mnem, i.kind, i.cc = 'db', 'dbcc', cond
            i.target = (i.addr + 2 + d) & 0xFFFFFFFF
            i.ops = [('d', reg), ('pcd', i.target)]
            i.sz = 'w'
            return
        dst = ea_op(r, mode, reg, 'b')
        if not is_data_alterable(dst):
            raise Illegal('scc')
        i.mnem, i.cc, i.sz, i.ops = 's', cond, 'b', [dst]
        return
    sz = SZ[szf]
    data = dn or 8
    dst = ea_op(r, mode, reg, sz)
    if dst[0] == 'a':
        if sz == 'b':
            raise Illegal('addq.b An')
    elif not is_data_alterable(dst):
        raise Illegal('addq dst')
    i.mnem = 'subq' if op & 0x100 else 'addq'
    i.sz = sz
    i.ops = [('imm', data), dst]


def _line8(r, i, op, mode, reg, dn):
    opm = (op >> 6) & 7
    if opm == 3 or opm == 7:
        src = ea_op(r, mode, reg, 'w')
        if src[0] == 'a':
            raise Illegal('div')
        i.mnem = 'divu' if opm == 3 else 'divs'
        i.sz = 'w'
        i.ops = [src, ('d', dn)]
        return
    if opm == 4 and mode in (0, 1):
        i.mnem, i.sz = 'sbcd', 'b'
        i.ops = [('d', reg), ('d', dn)] if mode == 0 else [('pre', reg), ('pre', dn)]
        return
    _logic(r, i, op, mode, reg, dn, 'or')


def _logic(r, i, op, mode, reg, dn, name):
    opm = (op >> 6) & 7
    sz = SZ[opm & 3]
    ea = ea_op(r, mode, reg, sz)
    if opm < 3:
        if ea[0] == 'a':
            raise Illegal('logic src An')
        i.ops = [ea, ('d', dn)]
    else:
        if not is_memory_alterable(ea):
            raise Illegal('logic dst')
        i.ops = [('d', dn), ea]
    i.mnem, i.sz = name, sz


def _addsub(r, i, op, mode, reg, dn, name):
    opm = (op >> 6) & 7
    if opm == 3 or opm == 7:
        sz = 'w' if opm == 3 else 'l'
        src = ea_op(r, mode, reg, sz)
        i.mnem, i.sz, i.ops = name + 'a', sz, [src, ('a', dn)]
        return
    sz = SZ[opm & 3]
    if opm >= 4 and mode in (0, 1):
        i.mnem, i.sz = name + 'x', sz
        i.ops = [('d', reg), ('d', dn)] if mode == 0 else [('pre', reg), ('pre', dn)]
        return
    ea = ea_op(r, mode, reg, sz)
    if opm < 3:
        if ea[0] == 'a' and sz == 'b':
            raise Illegal('addsub.b An')
        i.ops = [ea, ('d', dn)]
    else:
        if not is_memory_alterable(ea):
            raise Illegal('addsub dst')
        i.ops = [('d', dn), ea]
    i.mnem, i.sz = name, sz


def _lineb(r, i, op, mode, reg, dn):
    opm = (op >> 6) & 7
    if opm == 3 or opm == 7:
        sz = 'w' if opm == 3 else 'l'
        src = ea_op(r, mode, reg, sz)
        i.mnem, i.sz, i.ops = 'cmpa', sz, [src, ('a', dn)]
        return
    sz = SZ[opm & 3]
    if opm < 3:
        ea = ea_op(r, mode, reg, sz)
        if ea[0] == 'a' and sz == 'b':
            raise Illegal('cmp.b An')
        i.mnem, i.sz, i.ops = 'cmp', sz, [ea, ('d', dn)]
        return
    if mode == 1:
        i.mnem, i.sz, i.ops = 'cmpm', sz, [('post', reg), ('post', dn)]
        return
    ea = ea_op(r, mode, reg, sz)
    if not is_data_alterable(ea):
        raise Illegal('eor')
    i.mnem, i.sz, i.ops = 'eor', sz, [('d', dn), ea]


def _linec(r, i, op, mode, reg, dn):
    opm = (op >> 6) & 7
    if opm == 3 or opm == 7:
        src = ea_op(r, mode, reg, 'w')
        if src[0] == 'a':
            raise Illegal('mul')
        i.mnem = 'mulu' if opm == 3 else 'muls'
        i.sz = 'w'
        i.ops = [src, ('d', dn)]
        return
    if opm == 4 and mode in (0, 1):
        i.mnem, i.sz = 'abcd', 'b'
        i.ops = [('d', reg), ('d', dn)] if mode == 0 else [('pre', reg), ('pre', dn)]
        return
    if (op & 0x1F8) == 0x140:
        i.mnem, i.sz, i.ops = 'exg', 'l', [('d', dn), ('d', reg)]
        return
    if (op & 0x1F8) == 0x148:
        i.mnem, i.sz, i.ops = 'exg', 'l', [('a', dn), ('a', reg)]
        return
    if (op & 0x1F8) == 0x188:
        i.mnem, i.sz, i.ops = 'exg', 'l', [('d', dn), ('a', reg)]
        return
    _logic(r, i, op, mode, reg, dn, 'and')


def _linee(r, i, op, mode, reg, dn):
    names = ['as', 'ls', 'rox', 'ro']
    left = bool(op & 0x100)
    if ((op >> 6) & 3) == 3:
        if op & 0x800:
            raise Illegal('shift mem')
        ea = ea_op(r, mode, reg, 'w')
        if not is_memory_alterable(ea):
            raise Illegal('shift mem dst')
        i.mnem = names[(op >> 9) & 3] + ('l' if left else 'r')
        i.sz = 'w'
        i.ops = [ea]
        return
    sz = SZ[(op >> 6) & 3]
    i.mnem = names[(op >> 3) & 3] + ('l' if left else 'r')
    i.sz = sz
    if op & 0x20:
        i.ops = [('d', dn), ('d', reg)]
    else:
        i.ops = [('imm', dn or 8), ('d', reg)]


# ---------------------------------------------------------------- printing
def hexs(v):
    return '$%X' % v


def fop(o):
    k = o[0]
    if k == 'd':
        return 'd%d' % o[1]
    if k == 'a':
        return 'a%d' % o[1]
    if k == 'ind':
        return '(a%d)' % o[1]
    if k == 'post':
        return '(a%d)+' % o[1]
    if k == 'pre':
        return '-(a%d)' % o[1]
    if k == 'disp':
        return '%d(a%d)' % (o[2], o[1])
    if k == 'idx':
        return '%d(a%d,%s%d.%s)' % (o[2], o[1], 'a' if o[4] else 'd', o[3], 'l' if o[5] else 'w')
    if k == 'absw':
        return '(%s).w' % hexs(o[1])
    if k == 'absl':
        return '(%s).l' % hexs(o[1])
    if k == 'pcd':
        return '%s(pc)' % hexs(o[1])
    if k == 'pci':
        return '%s(pc,%s%d.%s)' % (hexs(o[1]), 'a' if o[3] else 'd', o[2], 'l' if o[4] else 'w')
    if k == 'imm':
        return '#%s' % hexs(o[1])
    if k == 'sr':
        return 'sr'
    if k == 'ccr':
        return 'ccr'
    if k == 'usp':
        return 'usp'
    if k == 'list':
        return 'list(%04X%s)' % (o[1], 'r' if o[2] else '')
    return str(o)


def fmt(i):
    m = i.mnem
    if i.cc is not None:
        m += CC[i.cc]
    if i.sz and i.sz not in ('s',):
        m += '.' + i.sz
    elif i.sz == 's':
        m += '.s'
    return '%-8s %s' % (m, ','.join(fop(o) for o in i.ops))
