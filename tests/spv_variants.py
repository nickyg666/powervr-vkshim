#!/usr/bin/env python3
"""Generate SPIR-V variants of a shader and test which ones the PowerVR driver
accepts."""
import re
import struct
import subprocess
import sys
import os

SRC = sys.argv[1] if len(sys.argv) > 1 else "/tmp/vkcompat/spv/0160-fragment.spv"
OUT = "/tmp/spvvar"
os.makedirs(OUT, exist_ok=True)

STRIP = {2, 3, 5, 6, 7, 8, 317, 330}


def load(path):
    d = open(path, "rb").read()
    return list(struct.unpack("<%dI" % (len(d) // 4), d))


def save(path, w):
    open(path, "wb").write(struct.pack("<%dI" % len(w), *w))


def rewrite(w, strip=False, kill2ret=False, drop_fdepth=False, version=None,
            drop_demote=False):
    out = list(w[:5])
    if version is not None:
        out[1] = version
    # find gl_FragDepth
    fdvar = 0
    i = 5
    while i < len(w):
        op = w[i] & 0xFFFF
        ln = w[i] >> 16
        if not ln:
            break
        if op == 71 and ln >= 4 and w[i + 2] == 11 and w[i + 3] == 22:
            fdvar = w[i + 1]
        i += ln
    i = 5
    while i < len(w):
        op = w[i] & 0xFFFF
        ln = w[i] >> 16
        if not ln:
            out.extend(w[i:])
            break
        skip = False
        if strip and op in STRIP:
            skip = True
        if not skip and drop_fdepth and op == 62 and ln >= 3 and fdvar and w[i + 1] == fdvar:
            skip = True
        if not skip and drop_demote and op in (5380,):
            skip = True
        if skip:
            i += ln
            continue
        out.extend(w[i:i + ln])
        if kill2ret and op == 252:
            out[-ln] = 253 | (ln << 16)
        i += ln
    return out


def run(vs, fs, n_out=1, fmt=0x25, blend=0, dfmt=0, dtest=0):
    cmd = ["/home/orangepi/.local/src/pipetest", str(n_out), hex(fmt), str(blend),
           hex(dfmt), str(dtest), vs, fs, "4"]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    line = (p.stdout or "").strip().splitlines()[-1] if p.stdout else ""
    m = re.search(r"-> (-?\d+)", line)
    rc = p.returncode
    if rc == 139:
        return "CRASH"
    if rc == 124:
        return "HANG"
    return "r=%s" % (m.group(1) if m else "?")


base = load(SRC)
variants = {
    "orig": base,
    "strip": rewrite(base, strip=True),
    "kill2ret": rewrite(base, kill2ret=True),
    "nofdepth": rewrite(base, drop_fdepth=True),
    "strip+kill": rewrite(base, strip=True, kill2ret=True),
    "strip+fdepth": rewrite(base, strip=True, drop_fdepth=True),
    "all": rewrite(base, strip=True, kill2ret=True, drop_fdepth=True),
    "v1.0": rewrite(base, version=0x00010000),
    "v1.3": rewrite(base, version=0x00010300),
    "v1.0+all": rewrite(base, version=0x00010000, strip=True, kill2ret=True, drop_fdepth=True),
    "v1.3+all": rewrite(base, version=0x00010300, strip=True, kill2ret=True, drop_fdepth=True),
    "v1.4+all": rewrite(base, version=0x00010400, strip=True, kill2ret=True, drop_fdepth=True),
}

vs = "/tmp/min_vs.spv"
for name, w in variants.items():
    p = os.path.join(OUT, name + ".spv")
    save(p, w)
    print("%-12s words=%-5d -> %s" % (name, len(w), run(vs, p)))
    sys.stdout.flush()
