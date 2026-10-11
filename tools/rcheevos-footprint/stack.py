import re,sys,subprocess,glob,functools
# Worst-case stack depth from a linked ARM ELF: edges from bl/blx in the disassembly,
# frame sizes from GCC .su files (rcheevos) or prologue push/sub sp (newlib/libgcc).
d=sys.argv[1]; elf=d+"/dscheevos_probe.elf"
su={}
for f in glob.glob(d+"/*/*.su")+glob.glob(d+"/*.su"):
    for ln in open(f):
        p=ln.rstrip("\n").split("\t")
        name=p[0].rsplit(":",1)[1].split(".")[0]
        su[name]=max(su.get(name,0),int(p[1]))
dis=subprocess.run(["arm-none-eabi-objdump","-d","--no-show-raw-insn",elf],capture_output=True,text=True).stdout
funcs={}; cur=None
for ln in dis.splitlines():
    m=re.match(r'^[0-9a-f]+ <([^>]+)>:',ln)
    if m: cur=m.group(1); funcs[cur]={"calls":set(),"push":0,"sub":0,"ind":False,"n":0}; continue
    if not cur or not ln.strip(): continue
    f=funcs[cur]; f["n"]+=1
    m=re.search(r'\t(bl|blx)\s+[0-9a-f]+ <([^>+]+)>',ln)
    if m: f["calls"].add(m.group(2))
    if re.search(r'\tblx\s+r\d',ln): f["ind"]=True
    if f["n"]<=16:
        m=re.search(r'\t(push|stmdb\s+sp!,)\s*\{([^}]*)\}',ln)
        if m: f["push"]+=4*len([r for r in m.group(2).split(",") if r.strip()])
        m=re.search(r'\tsub\s+sp,\s*(?:sp,\s*)?#(\d+)',ln)
        if m: f["sub"]+=int(m.group(1))
def frame(n):
    b=n.split(".")[0]
    if b in su: return su[b],"su"
    return funcs[n]["push"]+funcs[n]["sub"],"prologue"
import os
EXCL=set(filter(None,os.environ.get("EXCL","").split(",")))
sys.setrecursionlimit(10000)
@functools.lru_cache(None)
def depth(n,stack=()):
    if n not in funcs: return 0,[n+"?"],False
    if n in EXCL: return 0,[n+" (cut)"],False
    if n in stack: return 0,[n+" (RECURSION)"],True
    fr,_=frame(n); best=(0,[],False)
    for c in funcs[n]["calls"]:
        r=depth(c,stack+(n,))
        if r[0]>best[0]: best=r
    rec=best[2] or any(depth(c,stack+(n,))[2] for c in funcs[n]["calls"])
    return fr+best[0],[n]+best[1],rec
for root in sys.argv[2:]:
    t,path,rec=depth(root)
    print("== %s: worst case %d bytes%s"%(root,t," (recursion on some path)" if rec else ""))
    print("   "+" -> ".join("%s[%d%s]"%(p,frame(p)[0] if p in funcs else 0,"" if p not in funcs or frame(p)[1]=="su" else "~") for p in path))
    ind=sorted({n for n in funcs if funcs[n]["ind"] and n.startswith("rc_")})
print("rc functions with indirect calls (callbacks; add host frames on top):", len(ind))
