#!/usr/bin/env python3
"""Classify the bare DISPCNT A reads (no RMW write follows) that hit the strip
window, by disassembling the code the runner saved around each read PC
(needs capstone). For each PC: does the code after the read keep the mode bits
(16-17) somewhere other than DISPCNT?

usage: bare_reads.py RUNDIR [--show]
  class "mode-extract": an AND/TST/BIC mask or shift pair that keeps bits 16-17,
         or the loaded value stored to memory unmasked (whole-register save);
  class "other-bits":   only masks that drop bits 16-17 (e.g. a layer getter);
  class "unclear":      neither seen in the next 10 instructions.
"""
import json
import os
import sys

import capstone


def classify(code_words, pc):
    base = (pc & ~3) - 0x20
    thumb = pc & 1
    data = b"".join(int(w, 16).to_bytes(4, "little") for w in code_words.split())
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB if thumb else capstone.CS_MODE_ARM)
    md.detail = True
    insns = list(md.disasm(data, base))
    idx = next((i for i, ins in enumerate(insns) if ins.address == (pc & ~1)), None)
    if idx is None:
        return "unclear", []
    ld = insns[idx]
    reg = ld.op_str.split(",")[0].strip()
    tracked = {reg}
    cls = "unclear"
    lines = [f"{ins.address:08x} {ins.mnemonic} {ins.op_str}" for ins in insns[idx:idx + 10]]
    for ins in insns[idx + 1:idx + 11]:
        ops = [o.strip() for o in ins.op_str.split(",")]
        srcs = set(ops[1:])
        if not tracked & set(o.strip("[]!") for o in ops):
            continue
        m = ins.mnemonic
        imm = None
        for o in ops:
            if o.startswith("#"):
                try:
                    imm = int(o[1:], 0)
                except ValueError:
                    pass
        if m.startswith(("and", "tst")) and imm is not None:
            cls = "mode-extract" if imm & 0x30000 else "other-bits"
            break
        if m.startswith("bic") and imm is not None:
            cls = "other-bits" if imm & 0x30000 == 0x30000 else "mode-extract"
            break
        if m.startswith(("lsl", "mov")) and "lsl #14" in ins.op_str or m.startswith("lsl") and "#14" in ins.op_str:
            cls = "mode-extract"
            break
        if m.startswith(("lsr", "mov")) and ("lsr #16" in ins.op_str or "#16" in ops[-1:]):
            cls = "mode-extract"
            break
        if m.startswith("str") and ops and ops[0] in tracked:
            cls = "mode-extract"  # whole-register save
            break
        if m.startswith(("mov", "orr", "add")) and srcs & tracked and ops:
            tracked.add(ops[0])
    return cls, lines


def main():
    rundir = sys.argv[1]
    show = "--show" in sys.argv
    for fn in sorted(os.listdir(rundir)):
        if not fn.endswith(".json"):
            continue
        j = json.load(open(os.path.join(rundir, fn)))
        out = []
        for b in j.get("bare_reads", []):
            if not b["win"]:
                continue
            cls, lines = classify(b["code_at_pc_minus_20"], int(b["pc"], 16))
            out.append((cls, b["pc"], b["win"], lines))
        if not out:
            continue
        tot = {}
        for c, _, w, _ in out:
            tot[c] = tot.get(c, 0) + w
        print(f"{fn[:-5]}\t" + "\t".join(f"{k}={v}" for k, v in sorted(tot.items())))
        if show:
            for c, pc, w, lines in out:
                print(f"  {pc} x{w} {c}")
                for l in lines[:6]:
                    print(f"     {l}")


if __name__ == "__main__":
    main()
