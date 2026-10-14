"""Control-flow analysis for the translator: successors, entries, function regions, stack balance."""
import sys, os, collections, pickle, json
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m68k, descent

# Computed jumps that are not bra.w tables (found by reading the code, see notes)
RUN_JUMPS = {            # jmp site -> inclusive-start, exclusive-end of the straight-line run it enters
    0x12F0: (0x1384, 0x178C),
    0x1BEC: (0x1BF0, 0x1CF2),
}
ABS_JUMPS = {            # jmp (a3) inside the 0x1BF0 run: a3 is always the loop head set at 0x1BC4
    0x1C42: 0x1BEA, 0x1C6E: 0x1BEA, 0x1CF0: 0x1BEA,
}
RAM_ALIAS = {0xFFFF045A: 0x721C, 0xFFFF048E: 0x7250, 0xFFFF049A: 0x725C}   # RAM copies of ROM images
ROOTS_EXTRA = [0x721C, 0x7250, 0x725C, 0x5E96, 0x5ED8]


def build():
    insns, why, unresolved, tables, illegal, ptrvar = descent.descend()
    # unresolved list must be fully covered by the special tables
    for a, r, o in unresolved:
        assert a in RUN_JUMPS or a in ABS_JUMPS, hex(a)
    order = sorted(insns)
    nxt = {}
    for a in order:
        nxt[a] = a + insns[a].size

    table_entries = {}
    for site, (s, tg) in tables.items():
        table_entries[site] = [s + 4 * k for k in range(len(tg))]

    run_words = {}
    for site, (lo, hi) in RUN_JUMPS.items():
        run_words[site] = [a for a in order if lo <= a < hi]

    succ = {}
    callees = collections.defaultdict(set)     # call site -> entry addresses
    for a in order:
        i = insns[a]
        k = i.kind
        n = nxt[a]
        s = []
        if k == 'seq' or k == 'trap':
            s = [n]
        elif k == 'bra':
            s = [i.target]
        elif k in ('bcc', 'dbcc'):
            s = [i.target, n]
        elif k in ('bsr', 'jsr'):
            s = [n]
            if i.target is not None:
                callees[a].add(RAM_ALIAS.get(i.target, i.target))
            elif a in table_entries:
                callees[a].update(table_entries[a])
            else:
                raise Exception('unresolved call %X' % a)
        elif k == 'jmp':
            if i.target is not None:
                s = [i.target]
            elif a in table_entries:
                s = list(table_entries[a])
            elif a in RUN_JUMPS:
                s = list(run_words[a])
            elif a in ABS_JUMPS:
                s = [ABS_JUMPS[a]]
            else:
                raise Exception('unresolved jmp %X' % a)
        elif k in ('rts', 'rte', 'rtr', 'stop'):
            s = []
        else:
            raise Exception('kind ' + k)
        for t in s:
            assert t in insns, (hex(a), hex(t))
        succ[a] = s

    roots = set()
    roots.add(descent.u32(4))
    for v in range(2, 64):
        t = descent.u32(v * 4)
        if 0x200 <= t < descent.ROMEND and not t & 1:
            roots.add(t)
    roots.update(ptrvar)
    roots.update(ROOTS_EXTRA)
    entries = set(roots)
    for a, ts in callees.items():
        entries.update(ts)
    return dict(insns=insns, order=order, succ=succ, callees=callees, tables=tables,
                table_entries=table_entries, roots=roots, entries=entries, ptrvar=ptrvar)


def regions(g):
    succ = g['succ']
    entries = set(g['entries'])
    preds = collections.defaultdict(set)
    for a, ss in succ.items():
        for t in ss:
            preds[t].add(a)
    splits = 0
    while True:
        reg = {}
        owners = collections.defaultdict(set)
        for e in entries:
            seen = {e}
            st = [e]
            while st:
                a = st.pop()
                for t in succ[a]:
                    if t in entries and t != e:
                        continue          # tail call into another function
                    if t not in seen:
                        seen.add(t)
                        st.append(t)
            reg[e] = seen
            for a in seen:
                owners[a].add(e)
        shared = {a for a, o in owners.items() if len(o) > 1}
        if not shared:
            break
        new = set()
        for a in shared:
            if not any(p in shared for p in preds[a]) or a in entries:
                new.add(a)
        if not new:
            new.add(min(shared))
        new -= entries
        if not new:
            new.add(min(shared - entries))
        entries |= new
        splits += len(new)
    return reg, entries, splits


# ------------------------------------------------------------------ stack
SZB = {'b': 2, 'w': 2, 'l': 4}


def popcount(x):
    return bin(x).count('1')


def sp_effect(i):
    """Return (delta, ok).  delta = change of A7 caused by this instruction (bytes)."""
    d = 0
    ops = i.ops
    m = i.mnem
    ok = True
    if m == 'movem':
        for o in ops:
            if o[0] == 'pre' and o[1] == 7:
                mask = [x for x in ops if x[0] == 'list'][0][1]
                d -= popcount(mask) * (4 if i.sz == 'l' else 2)
            elif o[0] == 'post' and o[1] == 7:
                mask = [x for x in ops if x[0] == 'list'][0][1]
                d += popcount(mask) * (4 if i.sz == 'l' else 2)
        return d, ok
    if m == 'pea':
        d -= 4
    for o in ops:
        if o[0] == 'pre' and o[1] == 7:
            d -= SZB.get(i.sz or 'l', 4)
        elif o[0] == 'post' and o[1] == 7:
            d += SZB.get(i.sz or 'l', 4)
    if ops and ops[-1] == ('a', 7) and m in ('addq', 'subq', 'adda', 'suba', 'lea', 'movea', 'exg', 'move'):
        if m in ('addq', 'subq'):
            v = ops[0][1]
            d += v if m == 'addq' else -v
        elif m in ('adda', 'suba'):
            if ops[0][0] == 'imm':
                v = ops[0][1]
                if i.sz == 'w':
                    v = m68k.s16(v)
                else:
                    v = m68k.s32(v)
                d += v if m == 'adda' else -v
            else:
                ok = False
        elif m == 'lea':
            if ops[0][0] == 'disp' and ops[0][1] == 7:
                d += ops[0][2]
            else:
                ok = False
        else:
            ok = False
    if m in ('link', 'unlk'):
        ok = False
    return d, ok


def stack_check(g, reg):
    insns, succ = g['insns'], g['succ']
    entries = set(reg)
    problems = collections.defaultdict(list)
    delta_at = {}
    stats = collections.Counter()
    for e, region in reg.items():
        dl = {e: 0}
        st = [e]
        bad = False
        while st:
            a = st.pop()
            i = insns[a]
            dv, ok = sp_effect(i)
            if not ok:
                problems[e].append((a, 'unknown sp change', str(i)))
                bad = True
            out = dl[a] + dv
            k = i.kind
            if k == 'rts' and out != 0:
                problems[e].append((a, 'rts with sp delta %d' % out, str(i)))
            if k == 'rte' and out != 0:
                problems[e].append((a, 'rte with sp delta %d' % out, str(i)))
            for t in succ[a]:
                if t in entries and t != e:
                    if out != 0:
                        problems[e].append((a, 'tail jump with sp delta %d to %X' % (out, t), str(i)))
                    continue
                if t in dl:
                    if dl[t] != out:
                        problems[e].append((a, 'sp delta merge conflict %d vs %d at %X' % (dl[t], out, t), str(i)))
                else:
                    dl[t] = out
                    st.append(t)
        delta_at[e] = dl
        stats['functions'] += 1
        if e in problems:
            stats['with problems'] += 1
    return problems, delta_at, stats


if __name__ == '__main__':
    g = build()
    print('insns', len(g['order']), 'roots', len(g['roots']), 'call entries', len(g['entries']))
    reg, entries, splits = regions(g)
    g['reg'] = reg
    g['all_entries'] = entries
    print('functions', len(reg), 'tail splits added', splits)
    tail_edges = 0
    for e, r in reg.items():
        for a in r:
            for t in g['succ'][a]:
                if t in reg and t != e:
                    tail_edges += 1
    print('tail-call edges', tail_edges)
    sizes = sorted((len(r) for r in reg.values()), reverse=True)
    print('largest regions (insns):', sizes[:8])
    problems, delta_at, stats = stack_check(g, reg)
    print(dict(stats))
    kinds = collections.Counter()
    for e, ps in problems.items():
        for p in ps:
            kinds[p[1].split(' with')[0].split(' to ')[0][:28]] += 1
    print('problem kinds', dict(kinds))
    for e, ps in sorted(problems.items())[:25]:
        print('FUNC %X: %d problems; first: %X %s | %s' % (e, len(ps), ps[0][0], ps[0][1], ps[0][2]))
    pickle.dump({'problems': dict(problems)}, open('stack_problems.pkl', 'wb'))
