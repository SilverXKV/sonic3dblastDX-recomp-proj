"""Driver: build CFG, emit C for every function, write funcs.h / s3d_*.c / dispatch.c"""
import sys, os, os, collections
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m68k, analyze, emit, descent

OUT = sys.argv[1] if len(sys.argv) > 1 else 'gen_out'
os.makedirs(OUT, exist_ok=True)
g = analyze.build()
reg, entries, splits = analyze.regions(g)
g['reg'] = reg
print('functions', len(reg))

# RAM code images: ram address -> (rom entry, length)
IMAGES = {
    0xFFFF045A: 0x721C, 0xFFFF048E: 0x7250, 0xFFFF049A: 0x725C,
}
HBI = {0x5E96: 44, 0x5ED8: 8}
img_len = {}
for e in list(IMAGES.values()) + list(HBI):
    r = reg[e]
    end = max(a + g['insns'][a].size for a in r)
    img_len[e] = end - e
print('image extents', {hex(k): v for k, v in img_len.items()})

funcs = {}
errors = []
for e in sorted(reg):
    try:
        funcs[e] = emit.FuncGen(g, e, reg[e], entries).emit()
    except Exception as ex:
        errors.append((e, repr(ex)))
print('emit errors', len(errors))
for e, x in errors[:30]:
    print(hex(e), x)
if errors:
    sys.exit(1)

with open(OUT + '/funcs.h', 'w') as f:
    f.write('#ifndef FUNCS_H\n#define FUNCS_H\n#include "m68k_rt.h"\n')
    for e in sorted(funcs):
        f.write('u32 f_%06X(void);\n' % e)
    f.write('#endif\n')

per = 40
es = sorted(funcs)
nfiles = 0
for k in range(0, len(es), per):
    with open(OUT + '/s3d_%02d.c' % nfiles, 'w') as f:
        f.write('#include "funcs.h"\n\n')
        for e in es[k:k + per]:
            f.write(funcs[e] + '\n\n')
    nfiles += 1

with open(OUT + '/dispatch.c', 'w') as f:
    f.write('#include "funcs.h"\n')
    f.write('/* generated: address -> translated function */\n')
    f.write('int call_addr_rom(u32 a, u32 *ret) {\n  switch (a) {\n')
    for e in es:
        f.write('  case 0x%06Xu: *ret = f_%06X(); return 1;\n' % (e, e))
    f.write('  default: return 0;\n  }\n}\n')
    f.write('/* RAM code images: ram address, rom entry, length */\n')
    f.write('const struct ramimg { u32 ram; u32 rom; u32 len; } ramimgs[] = {\n')
    for ra, re_ in IMAGES.items():
        f.write('  {0x%Xu, 0x%Xu, %d},\n' % (ra, re_, img_len[re_]))
    f.write('};\nconst int nramimgs = %d;\n' % len(IMAGES))
    f.write('const struct hbiimg { u32 rom; u32 len; } hbiimgs[] = {\n')
    for re_, ln in HBI.items():
        f.write('  {0x%Xu, %d},\n' % (re_, img_len[re_]))
    f.write('};\nconst int nhbiimgs = %d;\n' % len(HBI))
print('wrote', nfiles, 'files')
