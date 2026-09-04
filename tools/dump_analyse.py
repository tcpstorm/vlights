# dump_analyse.py - walk a FiveM minidump and symbolise every frame that falls
# inside VLights.asi. Usage: python dump_analyse.py <crash.dmp> <syms.txt>
# where syms.txt is `x86_64-w64-mingw32-nm -n --demangle` output of an
# unstripped build (see docs/diagnostics.md). Needs: pip install minidump

import sys, struct, bisect
from minidump.minidumpfile import MinidumpFile

path, syms_path = sys.argv[1], sys.argv[2]
IMAGE_BASE = 0x2da410000
syms = []
for line in open(syms_path, encoding='utf-8', errors='replace'):
    parts = line.strip().split(' ', 2)
    if len(parts) < 3:
        continue
    try:
        syms.append((int(parts[0], 16) - IMAGE_BASE, parts[2]))
    except ValueError:
        pass
syms.sort()
offs = [s[0] for s in syms]

def symbolize(off):
    i = bisect.bisect_right(offs, off) - 1
    return "?" if i < 0 else "%s+0x%X" % (syms[i][1], off - syms[i][0])

mf = MinidumpFile.parse(path)
mods = mf.modules.modules
f = open(path, 'rb')

def basename(n):
    return n.replace('\\', '/').split('/')[-1]

def mod_for(a):
    for m in mods:
        if m.baseaddress <= a < m.baseaddress + m.size:
            name = basename(m.name)
            off = a - m.baseaddress
            s = "%s+0x%X" % (name, off)
            if 'vlights' in name.lower():
                s += "  [" + symbolize(off) + "]"
            return s
    return None

def read_at(rva, size):
    f.seek(rva)
    return f.read(size)

regs = {'Rip': 0xF8, 'Rsp': 0x98, 'Rcx': 0x80, 'Rdx': 0x88, 'R8': 0xB8, 'R9': 0xC0, 'Rbx': 0x90, 'Rsi': 0xA8, 'Rdi': 0xB0, 'R12': 0xD8, 'R13': 0xE0, 'R14': 0xE8, 'R15': 0xF0}
interesting = []
for t in mf.threads.threads:
    ctx = read_at(t.ThreadContext.Rva, t.ThreadContext.DataSize)
    if len(ctx) < 0x100:
        continue
    rip = struct.unpack_from('<Q', ctx, 0xF8)[0]
    data = read_at(t.Stack.Rva, t.Stack.DataSize)
    hits = []
    for i in range(0, len(data) - 8, 8):
        v = struct.unpack_from('<Q', data, i)[0]
        m = mod_for(v)
        if m and ('lodlight' in m.lower() or 'GTAProcess' in m or 'MinHook' in m):
            hits.append((i, m))
    if any('lodlight' in h[1].lower() for h in hits):
        interesting.append((t.ThreadId, rip, ctx, hits))

print("threads:", len(mf.threads.threads), "with plugin frames:", len(interesting))
for tid, rip, ctx, hits in interesting:
    print("=== thread %d rip=%s" % (tid, mod_for(rip) or hex(rip)))
    for name, off in regs.items():
        v = struct.unpack_from('<Q', ctx, off)[0]
        print("   %-3s = 0x%016X %s" % (name, v, mod_for(v) or ''))
    for i, m in hits[:40]:
        print("   sp+0x%04X: %s" % (i, m))
