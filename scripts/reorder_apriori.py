#!/usr/bin/env python3
"""Reorder APRIORI.full.inc to follow the stock uefi_a APRIORI order.

Stock skeleton  : the 91 GUIDs decoded from the stock FV's APRIORI DXE section.
Our modules     : FVMAIN.Fv (GUID -> module name), matched by GUID, not by name.
Our INF lines   : profiles/DXE.full.inc (basename -> full INF line).

Matched modules are emitted in stock order. Modules we have that the stock list
does not mention are NOT dumped at the end: each is inserted right after the
nearest preceding matched module from the ORIGINAL order, so its original
position relative to its neighbours is preserved.

Usage (inside WSL):
  python3 reorder_apriori.py [--apply]
"""
import os, re, sys, uuid

EXT = "/mnt/l/Desktop/ProjectMu/extract"
REPO = "/mnt/l/Desktop/ProjectMu/mu_aloha_platforms_sm8650-main"
DEV = REPO + "/Platforms/ShennongPkg/Device/xiaomi-shennong"
APRIORI = DEV + "/profiles/APRIORI.full.inc"
FVLIST = "/root/fvlist.txt"

PINNED = ["dxemain", "pcd"]          # must stay first, stock starts with DxeCore

# Modules that must NOT be placed at their stock position.
#
# EnvDxe is 0x0010 in the stock APRIORI, immediately after DxeCore. Placing ours
# there crashes the boot before it reaches DXE dispatch completion:
#
#   Synchronous Exception at 0x00000000D491F210
#
# It needs runtime services, variable services and the security arch up first,
# which in our build means RuntimeDxe, ResetSystemRuntimeDxe, RngDxe,
# SecurityStubDxe and CpuDxe all have to precede it. The stock binary evidently
# has different prerequisites. Treat it as an extra so it keeps the position it
# has relative to its neighbours.
FORCE_EXTRA = {"envdxe"}


def norm(s):
    s = s.lower()
    for suf in ("driver", "dxe", "lib", "peim"):
        if s.endswith(suf) and len(s) > len(suf):
            s = s[: -len(suf)]
    return re.sub(r"[^a-z0-9]", "", s)


# ---- stock order -----------------------------------------------------------
raw = open(EXT + "/uefi_extract/fc510ee7-ffdc-11d4-bd41-0080c73c8881.raw", "rb").read()
gname = {}
for line in open(EXT + "/guid_map.txt", encoding="utf-8"):
    p = line.strip().split(",")
    if len(p) >= 2 and p[0].count("-") == 4:
        gname[p[0].upper()] = p[1]
stock = []
for i in range(0, len(raw) - 15, 16):
    g = str(uuid.UUID(bytes_le=raw[i:i + 16])).upper()
    stock.append(g)
stock_index = {g: i for i, g in enumerate(stock)}

# ---- our FV ----------------------------------------------------------------
fv_guid = {}
for line in open(FVLIST, encoding="utf-8"):
    m = re.search(r"([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})\s+(\S+)", line, re.I)
    if m:
        fv_guid[m.group(1).upper()] = m.group(2)

byname = {}
for g, n in fv_guid.items():
    byname.setdefault(norm(n), g)

# ---- INF lines -------------------------------------------------------------
lines = open(APRIORI, encoding="utf-8").read().split("\n")
entries = []                                  # (inf_line, guid_or_None)
for ln in lines:
    if re.match(r"^\s*INF ", ln) and ln.strip().lower().endswith(".inf"):
        base = ln.strip().split("/")[-1][:-4]
        entries.append((ln, byname.get(norm(base))))

matched = [(ln, g) for ln, g in entries if g in stock_index]
extras = [(ln, g) for ln, g in entries if g not in stock_index]

print("INF lines: %d   matched to stock: %d   extras: %d" % (len(entries), len(matched), len(extras)))

# ---- build the new order ---------------------------------------------------
# 1) matched modules, in stock order
pinned = [ln for ln, g in entries if norm(ln.strip().split("/")[-1][:-4]) in PINNED]
rest = [(ln, g) for ln, g in entries if ln not in pinned]
by_guid = {}
for ln, g in rest:
    if g in stock_index and norm(ln.strip().split("/")[-1][:-4]) not in FORCE_EXTRA:
        by_guid.setdefault(g, []).append(ln)

final = []
for g in stock:
    final.extend(by_guid.get(g, []))

# 2) extras: insert after the nearest preceding matched module of the ORIGINAL order
anchor_of = {}
last = None
for ln, g in rest:
    if g in stock_index and norm(ln.strip().split("/")[-1][:-4]) not in FORCE_EXTRA:
        last = g
    else:
        anchor_of[ln] = last

for ln, g in rest:
    if g in stock_index and norm(ln.strip().split("/")[-1][:-4]) not in FORCE_EXTRA:
        continue
    a = anchor_of.get(ln)
    if a is None:
        final.insert(0, ln)
    else:
        host = by_guid[a][-1]
        final.insert(final.index(host) + 1, ln)

print("pinned: %d   matched: %d   extras: %d   total: %d"
      % (len(pinned), sum(len(v) for v in by_guid.values()),
         sum(1 for _, g in rest if g not in stock_index), len(pinned) + len(final)))

if "-v" in sys.argv:
    print("--- not matched to a stock GUID (inserted after their original neighbour) ---")
    for ln, g in rest:
        if g not in stock_index:
            a = anchor_of.get(ln)
            print("   %-44s after %s" % (ln.strip().split("/")[-1][:-4],
                                          fv_guid.get(a, "TOP") if a else "TOP"))

if "--apply" in sys.argv:
    out = ["APRIORI DXE {",
           "    #",
           "    # Order follows the stock uefi_a APRIORI DXE section (91 entries, decoded",
           "    # with extract/decode_apriori.py). Shared modules sit at their stock",
           "    # position; modules the stock list does not mention are kept next to the",
           "    # neighbour they originally followed. Regenerate with",
           "    # tools/reorder_apriori.py --apply.",
           "    #"]
    out += [l.rstrip() for l in pinned]
    out += [l.rstrip() for l in final]
    out.append("}")
    open(APRIORI, "w", encoding="utf-8", newline="").write("\n".join(out) + "\n")
    print("written:", APRIORI)
else:
    print("\n--- planned order (first 34) ---")
    for l in (pinned + final)[:34]:
        print("   ", l.strip())
