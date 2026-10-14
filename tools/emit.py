"""68000 -> C emitter.

Every translated function is `u32 f_XXXXXX(void)`.  It returns the address its final RTS
(or RTE) popped from the emulated stack.  A call site pushes its return address, calls,
and compares: a mismatch means the callee returned to an outer frame (non-local return),
so the caller returns the same value upward.
"""
import sys, os, collections
sys.dont_write_bytecode = True
sys.path.insert(0, '.')
import m68k, analyze, timing
import patches

M = {'b': '0xFFu', 'w': '0xFFFFu', 'l': '0xFFFFFFFFu'}
BITS = {'b': 8, 'w': 16, 'l': 32}
NB = {'b': 1, 'w': 2, 'l': 4}
SHIFT_KIND = {'asl': 0, 'asr': 1, 'lsl': 2, 'lsr': 3, 'rol': 4, 'ror': 5, 'roxl': 6, 'roxr': 7}
CCN = ['T', 'F', 'HI', 'LS', 'CC', 'CS', 'NE', 'EQ', 'VC', 'VS', 'PL', 'MI', 'GE', 'LT', 'GT', 'LE']


def h(v):
    return '0x%Xu' % (v & 0xFFFFFFFF)


def fname(a):
    return 'f_%06X' % a


def sx(sz, e):
    if sz == 'b':
        return 'SX8(%s)' % e
    if sz == 'w':
        return 'SX16(%s)' % e
    return e


class Ctx:
    """Per-instruction emission context with temp-variable allocation."""

    def __init__(self, addr=None):
        self.pre = []
        self.n = 0
        self.addr = addr
        self.opn = 0

    def tmp(self):
        self.n += 1
        return 't%d' % self.n


def idx_expr(xr, xis_a, xlong):
    reg = ('A[%d]' if xis_a else 'D[%d]') % xr
    return reg if xlong else 'SX16(%s)' % reg


def ea_addr(c, o, sz):
    """Emit statements computing the effective address into a temp; return temp name.
    Handles predec/postinc side effects."""
    k = o[0]
    t = c.tmp()
    inc = NB[sz] if sz else 4
    if k == 'ind':
        c.pre.append('u32 %s = A[%d];' % (t, o[1]))
    elif k == 'post':
        n = o[1]
        step = 2 if (n == 7 and sz == 'b') else inc
        c.pre.append('u32 %s = A[%d]; A[%d] += %d;' % (t, n, n, step))
    elif k == 'pre':
        n = o[1]
        step = 2 if (n == 7 and sz == 'b') else inc
        c.pre.append('A[%d] -= %d; u32 %s = A[%d];' % (n, step, t, n))
    elif k == 'disp':
        c.pre.append('u32 %s = A[%d] + %s;' % (t, o[1], h(o[2])))
    elif k == 'idx':
        c.pre.append('u32 %s = A[%d] + %s + %s;' % (t, o[1], h(o[2]), idx_expr(o[3], o[4], o[5])))
    elif k in ('absw', 'absl', 'pcd'):
        c.pre.append('u32 %s = %s;' % (t, h(o[1])))
    elif k == 'pci':
        c.pre.append('u32 %s = %s + %s;' % (t, h(o[1]), idx_expr(o[2], o[3], o[4])))
    else:
        raise Exception('ea_addr ' + str(o))
    return t


class Operand:
    """Resolved operand: knows how to read and write itself."""

    def __init__(self, c, o, sz):
        self.o = o
        self.sz = sz
        self.k = o[0]
        self.addr = None
        self.adj = None
        self.immx = None
        ca = getattr(c, 'addr', None)
        if ca in patches.ADJ:
            ix = c.opn
            c.opn += 1
            al = patches.ADJ[ca]
            self.adj = al[ix] if ix < len(al) else None
        if self.k == 'imm' and ca in patches.IMM:
            self.immx = patches.IMM[ca]
        if self.k in ('ind', 'post', 'pre', 'disp', 'idx', 'absw', 'absl', 'pcd', 'pci'):
            self.addr = ea_addr(c, o, sz)

    def read(self):
        r = self._read()
        if self.adj:
            r = self.adj % r
        return r

    def _read(self):
        o, sz, k = self.o, self.sz, self.k
        if self.immx:
            return '(%s)' % self.immx
        if k == 'd':
            return 'D[%d]' % o[1] if sz == 'l' else '(D[%d] & %s)' % (o[1], M[sz])
        if k == 'a':
            return 'A[%d]' % o[1] if sz == 'l' else '(A[%d] & %s)' % (o[1], M[sz])
        if k == 'imm':
            return h(o[1])
        return '%s(%s)' % ({'b': 'rd8', 'w': 'rd16', 'l': 'rd32'}[sz], self.addr)

    def write(self, v):
        o, sz, k = self.o, self.sz, self.k
        if k == 'd':
            if sz == 'l':
                return 'D[%d] = %s;' % (o[1], v)
            return 'SET%s(D[%d], %s);' % (sz.upper(), o[1], v)
        if k == 'a':
            return 'A[%d] = %s;' % (o[1], v)
        if k == 'imm':
            raise Exception('write to imm')
        return '%s(%s, %s);' % ({'b': 'wr8', 'w': 'wr16', 'l': 'wr32'}[sz], self.addr, v)


class FuncGen:
    def __init__(self, g, entry, region, all_entries):
        self.g = g
        self.entry = entry
        self.region = region
        self.insns = g['insns']
        self.all_entries = all_entries
        self.lines = []
        self.labels = set()
        self.table_entries = g['table_entries']

    # ---------------------------------------------------------------- control helpers
    def target_stmt(self, t, cur, cyc_extra=0, check=False):
        """C statement that transfers control to address t (a branch/jump)."""
        pre = ''
        if cyc_extra:
            pre = 'CYC(%d); ' % cyc_extra
        if t in self.region and not (t in self.all_entries and t != self.entry):
            self.labels.add(t)
            back = t <= cur
            return '%s%sgoto L_%06X;' % (pre, 'CHECK(); ' if back else '', t)
        # another function: tail call
        return '%s%sreturn %s();' % (pre, 'CHECK(); ', fname(t))

    def call_stmts(self, t, ret):
        """call function t with return address ret"""
        return ['CHECK();', 'push32(%s);' % h(ret),
                '{ u32 r_ = %s(); if (r_ != %s) return r_; }' % (fname(t), h(ret))]

    # ---------------------------------------------------------------- emission
    def emit(self):
        order = sorted(self.region)
        out = []
        out.append('u32 %s(void) {' % fname(self.entry))
        out.append('  TRC(%s);' % h(self.entry))
        body = []
        if order[0] != self.entry:               # the entry point is not the lowest address of the region
            self.labels.add(self.entry)
            body.append('goto L_%06X;' % self.entry)
        pending_cycles = 0
        for idx, a in enumerate(order):
            i = self.insns[a]
            lines, falls = self.insn(i, a)
            next_a = order[idx + 1] if idx + 1 < len(order) else None
            blk = []
            blk.append('L_%06X: ;' % a)
            blk.append('{ /* %06X %s */' % (a, m68k.fmt(i)))
            blk.append('  POLL();')
            blk.extend('  ' + l for l in patches.PRE.get(a, []))
            blk.extend('  ' + l for l in lines)
            blk.extend('  ' + l for l in patches.POST.get(a, []))
            blk.append('}')
            if falls:
                n = a + i.size
                if next_a != n:
                    blk.append(self.target_stmt(n, a))
            body.extend(blk)
        # drop labels never referenced
        used = set(self.labels)
        txt = []
        for l in body:
            if l.startswith('L_') and l.endswith(': ;'):
                addr = int(l[2:8], 16)
                if addr not in used:
                    continue
            txt.append(l)
        out.extend(txt)
        out.append('}')
        return '\n'.join(out)

    # one instruction -> (list of C lines, falls_through)
    def insn(self, i, a):
        c = Ctx(a)
        body = []
        falls = True
        cyc = timing.cycles(i)
        m = i.mnem
        sz = i.sz
        ops = i.ops
        k = i.kind
        nxt = a + i.size

        def O(o, s=None):
            return Operand(c, o, s or sz)

        if m == 'nop':
            pass
        elif m == 'move':
            self.e_move(c, body, i)
        elif m == 'movea':
            s = O(ops[0])
            v = s.read()
            body.append('A[%d] = %s;' % (ops[1][1], sx(sz, v)))
        elif m == 'moveq':
            body.append('D[%d] = %s; (void)logic_l(D[%d]);' % (ops[1][1], h(ops[0][1]), ops[1][1]))
        elif m == 'lea':
            s = O(ops[0], 'l')
            body.append('A[%d] = %s;' % (ops[1][1], s.addr))
        elif m == 'pea':
            s = O(ops[0], 'l')
            body.append('push32(%s);' % s.addr)
        elif m == 'clr':
            d = O(ops[0])
            if d.addr is not None and ops[0][0] in ('absw', 'absl') and 0xA00000 <= (ops[0][1] & 0xFFFFFF) < 0xE00000:
                body.append('(void)%s;' % d.read())
            body.append(d.write('0'))
            body.append('fN = 0; fZ = 1; fV = 0; fC = 0;')
        elif m in ('add', 'sub', 'and', 'or', 'eor', 'addi', 'subi', 'andi', 'ori', 'eori'):
            self.e_binop(c, body, i)
        elif m in ('addq', 'subq'):
            d = O(ops[1])
            v = ops[0][1]
            if ops[1][0] == 'a':
                body.append('A[%d] %s= %d;' % (ops[1][1], '+' if m == 'addq' else '-', v))
            else:
                f = 'add' if m == 'addq' else 'sub'
                body.append('{ u32 r_ = %s_%s(%d, %s); %s }' % (f, sz, v, d.read(), d.write('r_')))
        elif m in ('adda', 'suba'):
            s = O(ops[0])
            body.append('A[%d] %s= %s;' % (ops[1][1], '+' if m == 'adda' else '-', sx(sz, s.read())))
        elif m == 'cmpa':
            s = O(ops[0])
            body.append('cmp_l(%s, A[%d]);' % (sx(sz, s.read()), ops[1][1]))
        elif m in ('cmp', 'cmpi'):
            s = O(ops[0])
            d = O(ops[1])
            body.append('cmp_%s(%s, %s);' % (sz, s.read(), d.read()))
        elif m == 'cmpm':
            s = O(ops[0])
            d = O(ops[1])
            body.append('cmp_%s(%s, %s);' % (sz, s.read(), d.read()))
        elif m in ('addx', 'subx'):
            s = O(ops[0])
            d = O(ops[1])
            body.append('{ u32 r_ = %s_%s(%s, %s); %s }' % (m, sz, s.read(), d.read(), d.write('r_')))
        elif m in ('abcd', 'sbcd'):
            s = O(ops[0], 'b')
            d = O(ops[1], 'b')
            body.append('{ u32 r_ = %s(%s, %s); %s }' % (m, s.read(), d.read(), d.write('r_')))
        elif m == 'nbcd':
            d = O(ops[0], 'b')
            body.append('{ u32 r_ = nbcd(%s); %s }' % (d.read(), d.write('r_')))
        elif m in ('neg', 'negx', 'not'):
            d = O(ops[0])
            if m == 'not':
                body.append('{ u32 r_ = logic_%s(~%s); %s }' % (sz, d.read(), d.write('r_')))
            else:
                body.append('{ u32 r_ = %s_%s(%s); %s }' % (m, sz, d.read(), d.write('r_')))
        elif m == 'tst':
            d = O(ops[0])
            body.append('tst_%s(%s);' % (sz, d.read()))
        elif m == 'ext':
            n = ops[0][1]
            if sz == 'w':
                body.append('SETW(D[%d], SX8(D[%d] & 0xFFu)); (void)logic_w(D[%d]);' % (n, n, n))
            else:
                body.append('D[%d] = SX16(D[%d] & 0xFFFFu); (void)logic_l(D[%d]);' % (n, n, n))
        elif m == 'swap':
            n = ops[0][1]
            body.append('D[%d] = (D[%d] << 16) | (D[%d] >> 16); (void)logic_l(D[%d]);' % (n, n, n, n))
        elif m == 'exg':
            r1 = ('D[%d]' if ops[0][0] == 'd' else 'A[%d]') % ops[0][1]
            r2 = ('D[%d]' if ops[1][0] == 'd' else 'A[%d]') % ops[1][1]
            body.append('{ u32 t_ = %s; %s = %s; %s = t_; }' % (r1, r1, r2, r2))
        elif m in SHIFT_KIND:
            self.e_shift(c, body, i)
        elif m in ('mulu', 'muls', 'divu', 'divs'):
            s = O(ops[0], 'w')
            n = ops[1][1]
            fn = {'mulu': 'mulu_w', 'muls': 'muls_w', 'divu': 'divu_w', 'divs': 'divs_w'}[m]
            body.append('D[%d] = %s(%s, D[%d]);' % (n, fn, s.read(), n))
        elif m in ('btst', 'bchg', 'bclr', 'bset'):
            self.e_bit(c, body, i)
        elif m == 's':
            d = O(ops[0], 'b')
            body.append(d.write('(CC_%s) ? 0xFFu : 0u' % CCN[i.cc]))
        elif m == 'movem':
            self.e_movem(c, body, i)
        elif m == 'movep':
            self.e_movep(c, body, i)
        elif m in ('b', 'bra'):
            if m == 'bra':
                body.append(self.target_stmt(i.target, a, cyc_extra=0))
                falls = False
            else:
                taken = self.target_stmt(i.target, a)
                body.append('if (CC_%s) { CYC(%d); %s }' % (CCN[i.cc], timing.branch_taken(i), taken))
                body.append('CYC(%d);' % timing.branch_nottaken(i))
                cyc = 0
        elif m == 'db':
            n = ops[0][1]
            tgt = self.target_stmt(i.target, a)
            if i.cc == 0:     # dbt: never loops
                body.append('/* dbt */')
            else:
                cond = '' if i.cc == 1 else '!(CC_%s) && ' % CCN[i.cc]
                body.append('if (%s1) { u32 c_ = (D[%d] - 1) & 0xFFFFu; SETW(D[%d], c_); if (c_ != 0xFFFFu) { CYC(10); %s } else CYC(14); } else CYC(12);' % (cond, n, n, tgt))
                cyc = 0
        elif m == 'bsr' or m == 'jsr':
            body.extend(self.e_call(c, i, a))
            cyc = timing.cycles(i)
        elif m == 'jmp':
            falls = False
            body.extend(self.e_jmp(c, i, a))
        elif m == 'rts':
            body.append('return pop32();')
            falls = False
        elif m == 'rte':
            body.append('{ u16 sr_ = pop16(); u32 pc_ = pop32(); m68k_set_sr(sr_); return pc_; }')
            falls = False
        elif m == 'rtr':
            body.append('{ u16 cc_ = pop16(); u32 pc_ = pop32(); m68k_set_ccr(cc_); return pc_; }')
            falls = False
        else:
            raise Exception('unsupported %s' % m68k.fmt(i))
        lines = list(c.pre)
        # cycles: emit before the body, except for control flow which accounts itself
        if cyc:
            lines.insert(0, 'CYC(%d);' % cyc)
        lines.extend(body)
        return lines, falls

    # ------------------------------------------------------------------ move family
    def e_move(self, c, body, i):
        ops, sz = i.ops, i.sz
        s, d = ops
        if d[0] == 'sr':
            so = Operand(c, s, 'w')
            body.append('m68k_set_sr(%s);' % so.read())
            return
        if d[0] == 'ccr':
            so = Operand(c, s, 'w')
            body.append('m68k_set_ccr(%s);' % so.read())
            return
        if s[0] == 'sr':
            do = Operand(c, d, 'w')
            body.append(do.write('m68k_get_sr()'))
            return
        if d[0] == 'usp':
            body.append('m68k_other_sp = A[%d];' % s[1])
            return
        if s[0] == 'usp':
            body.append('A[%d] = m68k_other_sp;' % d[1])
            return
        so = Operand(c, s, sz)
        do = Operand(c, d, sz)
        body.append('{ u32 v_ = logic_%s(%s); %s }' % (sz, so.read(), do.write('v_')))

    def e_binop(self, c, body, i):
        ops, sz, m = i.ops, i.sz, i.mnem
        base = m[:-1] if m.endswith('i') and m not in ('add', 'sub') else m
        if m in ('addi',):
            base = 'add'
        if m == 'subi':
            base = 'sub'
        if m == 'andi':
            base = 'and'
        if m == 'ori':
            base = 'or'
        if m == 'eori':
            base = 'eor'
        s = Operand(c, ops[0], sz) if ops[0][0] not in ('ccr', 'sr') else None
        if ops[1][0] in ('ccr', 'sr'):
            val = ops[0][1]
            if ops[1][0] == 'ccr':
                cur, setter = 'm68k_get_ccr()', 'm68k_set_ccr'
            else:
                cur, setter = 'm68k_get_sr()', 'm68k_set_sr'
            op = {'and': '&', 'or': '|', 'eor': '^'}[base]
            body.append('%s(%s %s %s);' % (setter, cur, op, h(val)))
            return
        d = Operand(c, ops[1], sz)
        if base in ('add', 'sub'):
            body.append('{ u32 r_ = %s_%s(%s, %s); %s }' % (base, sz, s.read(), d.read(), d.write('r_')))
        else:
            op = {'and': '&', 'or': '|', 'eor': '^'}[base]
            body.append('{ u32 r_ = logic_%s(%s %s %s); %s }' % (sz, s.read(), op, d.read(), d.write('r_')))

    def e_shift(self, c, body, i):
        m, sz, ops = i.mnem, i.sz, i.ops
        kind = SHIFT_KIND[m]
        if len(ops) == 1:      # memory shift by 1, word
            d = Operand(c, ops[0], 'w')
            body.append('{ u32 r_ = m68k_shift(%d, 16, %s, 1); %s }' % (kind, d.read(), d.write('r_')))
            return
        cnt = ops[0]
        if cnt[0] == 'imm':
            cexp = str(cnt[1])
        else:
            cexp = '(D[%d] & 63u)' % cnt[1]
        d = Operand(c, ops[1], sz)
        body.append('{ u32 r_ = m68k_shift(%d, %d, %s, %s); %s }' % (kind, BITS[sz], d.read(), cexp, d.write('r_')))

    def e_bit(self, c, body, i):
        m, ops = i.mnem, i.ops
        src, dst = ops
        if src[0] == 'd':
            bit = 'D[%d]' % src[1]
        else:
            bit = str(src[1])
        if dst[0] == 'd':
            width = 31
            d = Operand(c, dst, 'l')
        else:
            width = 7
            d = Operand(c, dst, 'b')
        body.append('{ u32 v_ = %s; u32 b_ = (%s) & %du; fZ = ((v_ >> b_) & 1u) == 0;' % (d.read(), bit, width))
        if m == 'bchg':
            body.append('  %s' % d.write('v_ ^ (1u << b_)'))
        elif m == 'bclr':
            body.append('  %s' % d.write('v_ & ~(1u << b_)'))
        elif m == 'bset':
            body.append('  %s' % d.write('v_ | (1u << b_)'))
        body.append('}')

    def e_movem(self, c, body, i):
        ops, sz = i.ops, i.sz
        nb = NB[sz]
        if ops[0][0] == 'list':          # registers -> memory
            lst = ops[0]
            ea = ops[1]
            mask = lst[1]
            if ea[0] == 'pre':
                n = ea[1]
                # mask bit0 = A7 ... bit15 = D0 ; store from A7 down to D0
                regs = []
                for b in range(16):
                    if mask & (1 << b):
                        r = 15 - b           # register index 0..15 (D0..D7,A0..A7)
                        regs.append(r)
                body.append('{ u32 base_ = A[%d]; u32 p_ = base_;' % n)
                for r in sorted(regs, reverse=True):
                    reg = ('D[%d]' % r) if r < 8 else ('A[%d]' % (r - 8))
                    val = reg
                    if r == 8 + n:
                        val = 'base_'
                    body.append('  p_ -= %d; %s(p_, %s);' % (nb, 'wr32' if sz == 'l' else 'wr16', val))
                body.append('  A[%d] = p_; }' % n)
            else:
                addr = Operand(c, ea, sz).addr
                regs = [b for b in range(16) if mask & (1 << b)]
                body.append('{ u32 p_ = %s;' % addr)
                for r in regs:
                    reg = ('D[%d]' % r) if r < 8 else ('A[%d]' % (r - 8))
                    body.append('  %s(p_, %s); p_ += %d;' % ('wr32' if sz == 'l' else 'wr16', reg, nb))
                body.append('}')
        else:                              # memory -> registers
            ea = ops[0]
            lst = ops[1]
            mask = lst[1]
            regs = [b for b in range(16) if mask & (1 << b)]
            if ea[0] == 'post':
                n = ea[1]
                body.append('{ u32 p_ = A[%d];' % n)
            else:
                addr = Operand(c, ea, sz).addr
                body.append('{ u32 p_ = %s;' % addr)
            for r in regs:
                reg = ('D[%d]' % r) if r < 8 else ('A[%d]' % (r - 8))
                rd = 'rd32(p_)' if sz == 'l' else 'SX16(rd16(p_))'
                body.append('  %s = %s; p_ += %d;' % (reg, rd, nb))
            if ea[0] == 'post':
                body.append('  A[%d] = p_;' % ea[1])
            body.append('}')

    def e_movep(self, c, body, i):
        ops, sz = i.ops, i.sz
        nbytes = 2 if sz == 'w' else 4
        if ops[0][0] == 'disp':          # memory -> register
            addr = Operand(c, ops[0], 'b').addr
            n = ops[1][1]
            if sz == 'w':
                body.append('SETW(D[%d], (rd8(%s) << 8) | rd8(%s + 2));' % (n, addr, addr))
            else:
                body.append('D[%d] = ((u32)rd8(%s) << 24) | ((u32)rd8(%s + 2) << 16) | ((u32)rd8(%s + 4) << 8) | rd8(%s + 6);' % (n, addr, addr, addr, addr))
        else:
            n = ops[0][1]
            addr = Operand(c, ops[1], 'b').addr
            if sz == 'w':
                body.append('wr8(%s, D[%d] >> 8); wr8(%s + 2, D[%d]);' % (addr, n, addr, n))
            else:
                body.append('wr8(%s, D[%d] >> 24); wr8(%s + 2, D[%d] >> 16); wr8(%s + 4, D[%d] >> 8); wr8(%s + 6, D[%d]);' % (addr, n, addr, n, addr, n, addr, n))

    # ------------------------------------------------------------------ calls and jumps
    def e_call(self, c, i, a):
        ret = a + i.size
        t = i.target
        out = []
        if t is not None:
            if t >= 0x400000:
                real = analyze.RAM_ALIAS[t]
                out.append('ramcode_check(%s);' % h(t))
                t = real
            out.extend(self.call_stmts(t, ret))
            return out
        # computed call through a bra.w table
        o = i.ops[0]
        ents = self.table_entries[a]
        idx = ea_addr(c, o, 'l')
        out.append('CHECK();')
        out.append('push32(%s);' % h(ret))
        sw = ['{ u32 r_; switch (%s) {' % idx]
        for e in ents:
            sw.append('  case %s: r_ = %s(); break;' % (h(e), fname(e)))
        sw.append('  default: m68k_fatal("bad table call", %s); r_ = 0; }' % h(a))
        sw.append('  if (r_ != %s) return r_; }' % h(ret))
        out.extend(sw)
        return out

    def e_jmp(self, c, i, a):
        o = i.ops[0]
        out = []
        if i.target is not None:
            out.append(self.target_stmt(i.target, a))
            return out
        if a in analyze.ABS_JUMPS:
            out.append(self.target_stmt(analyze.ABS_JUMPS[a], a))
            return out
        if a in analyze.RUN_JUMPS:
            lo, hi = analyze.RUN_JUMPS[a]
            words = [x for x in sorted(self.insns) if lo <= x < hi]
            tgt = ea_addr(c, o, 'l')
            out.extend(c.pre)
            c.pre.clear()
            out.append('switch (%s) {' % tgt)
            for w in words:
                if w in self.region and not (w in self.all_entries and w != self.entry):
                    self.labels.add(w)
                    out.append('  case %s: goto L_%06X;' % (h(w), w))
                else:
                    out.append('  case %s: CHECK(); return %s();' % (h(w), fname(w)))
            out.append('  default: m68k_fatal("bad run jump target", %s); }' % tgt)
            return out
        ents = self.table_entries[a]
        tgt = ea_addr(c, o, 'l')
        out.extend(c.pre)
        c.pre.clear()
        out.append('switch (%s) {' % tgt)
        for e in ents:
            out.append('  case %s: %s' % (h(e), self.target_stmt(e, a)))
        out.append('  default: m68k_fatal("bad table jump", %s); }' % h(a))
        return out
