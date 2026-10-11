# Static call tree below a root in the linked ELF: direct calls from disassembly, inlined callees from DWARF,
# indirect call sites flagged. Prints reachable set grouped by origin, plus inlining per function.
import re,sys,subprocess,collections
from elftools.elf.elffile import ELFFile
elf_path,root=sys.argv[1],sys.argv[2]
dis=subprocess.run(["arm-none-eabi-objdump","-d","--no-show-raw-insn",elf_path],capture_output=True,text=True).stdout
calls=collections.defaultdict(set); ind=collections.Counter(); cur=None
for ln in dis.splitlines():
    m=re.match(r'^[0-9a-f]+ <([^>]+)>:',ln)
    if m: cur=m.group(1); continue
    if not cur: continue
    m=re.search(r'\tblx?\s+[0-9a-f]+ <([^>+]+)>',ln)
    if m: calls[cur].add(m.group(1))
    if re.search(r'\tblx\s+r\d',ln) or re.search(r'\tbl\s+[0-9a-f]+ <__gnu_thumb1_case|<__call_via_r',ln) : pass
    if re.search(r'\tblx\s+r\d|<__call_via_r\d>|<_call_via_r\d>',ln): ind[cur]+=1
sizes={}
for l in subprocess.run(["arm-none-eabi-nm","-S","-t","d",elf_path],capture_output=True,text=True).stdout.splitlines():
    p=l.split()
    if len(p)==4 and p[2] in "tTwW": sizes[p[3]]=int(p[1])
seen=set(); order=[]
def walk(f):
    if f in seen: return
    seen.add(f); order.append(f)
    for c in sorted(calls.get(f,())): walk(c)
walk(root)
# DWARF: inlined subroutines per concrete function
inl=collections.defaultdict(collections.Counter)
with open(elf_path,"rb") as fh:
    e=ELFFile(fh); dw=e.get_dwarf_info()
    for cu in dw.iter_CUs():
        def name_of(die):
            for attr in ("DW_AT_name",):
                if attr in die.attributes: return die.attributes[attr].value.decode()
            for ref in ("DW_AT_abstract_origin","DW_AT_specification"):
                if ref in die.attributes: return name_of(die.get_DIE_from_attribute(ref))
            return None
        def visit(die,owner):
            for ch in die.iter_children():
                if ch.tag=="DW_TAG_inlined_subroutine":
                    n=name_of(ch)
                    if owner and n: inl[owner][n]+=1
                    visit(ch,owner)
                elif ch.tag=="DW_TAG_subprogram":
                    visit(ch,name_of(ch) if "DW_AT_low_pc" in ch.attributes else None)
                else: visit(ch,owner)
        visit(cu.get_top_DIE(),None)
def origin(f):
    if f.startswith("rc_") or f.startswith("md5_"): return "rcheevos"
    if f.startswith("__aeabi") or f.startswith("__gnu") or re.match(r'__\w+[sd]f\d|__(udiv|div|mod|umod)', f) or f.startswith("__clz"): return "libgcc"
    return "newlib"
groups=collections.defaultdict(list)
for f in order: groups[origin(f)].append(f)
print("reachable from %s: %d functions"%(root,len(order)))
for g in ("rcheevos","newlib","libgcc"):
    fs=groups[g]; print("\n[%s] %d functions, %d bytes"%(g,len(fs),sum(sizes.get(f,0) for f in fs)))
    print("  "+", ".join(sorted(fs)))
print("\n[indirect call sites (function pointers) in reachable rcheevos code]")
for f in order:
    if ind[f] and origin(f)=="rcheevos": print("  %s: %d"%(f,ind[f]))
print("\n[functions inlined into reachable rcheevos code]")
for f in order:
    if inl.get(f): print("  %s <- %s"%(f,", ".join("%s%s"%(k," x%d"%v if v>1 else "") for k,v in sorted(inl[f].items()))))
