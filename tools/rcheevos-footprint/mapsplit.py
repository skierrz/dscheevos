import re,sys,collections,subprocess
# Sum kept input sections in a GNU ld map by origin. Input-section lines start with one space + '.';
# the address/size/file may be on the same line or the next.
lines=open(sys.argv[1]).read().split("Linker script and memory map",1)[1].splitlines()
out=None; cur=None
agg=collections.defaultdict(collections.Counter)
num=re.compile(r'^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)\s*$')
def origin(src):
    if "librc_" in src: return "rc/"+re.search(r'librc_(\w+)\.a',src).group(1)
    if "driver.o" in src: return "driver(stub)"
    if "libm.a" in src: return "newlib libm"
    if "libc.a" in src or "libg.a" in src or "nosys" in src: return "newlib libc"
    if "libgcc" in src: return "libgcc (softfp/div)"
    return "crt/other"
for ln in lines:
    m=re.match(r'^(\.\S+)',ln)
    if m: out=m.group(1); cur=None
    m=re.match(r'^ (\.\S+|COMMON)(.*)$',ln)
    if m:
        cur=m.group(1); rest=m.group(2)
        mm=num.match(rest) if rest.strip() else None
        if not mm: continue
    else:
        mm=num.match(ln) if cur else None
        if not mm: continue
    addr,size,src=int(mm.group(1),16),int(mm.group(2),16),mm.group(3)
    cur_sec=cur; cur=None
    if addr==0 or size==0 or out not in(".text",".rodata",".data",".bss"): continue
    kind=out[1:]
    if ".str1." in cur_sec:
        kind="str"
        if "driver.o" in src: continue  # ld puts the whole merged pool on the first object
    agg[origin(src)][kind]+=size
K=("text","rodata","str","data","bss")
print("%-20s"%"origin"+"".join("%9s"%k for k in K)+"%9s"%"total")
tot=collections.Counter()
for k in sorted(agg):
    c=agg[k]; tot.update(c); print("%-20s"%k+"".join("%9d"%c[x] for x in K)+"%9d"%sum(c[x] for x in K))
print("%-20s"%"TOTAL"+"".join("%9d"%tot[x] for x in K)+"%9d"%sum(tot[x] for x in K))
el=sys.argv[1].replace("link.map","dscheevos_probe.elf")
secs={m.group(1):int(m.group(2),16) for m in re.finditer(r'\]\s+(\.\w+)\s+\w+\s+\w+\s+\w+\s+(\w+)',subprocess.run(["arm-none-eabi-readelf","-SW",el],capture_output=True,text=True).stdout)}
pool=secs.get(".rodata",0)-tot["rodata"]
print("real merged string pool after dedup: %d (pre-dedup sum %d)"%(pool,tot["str"]))
print("ELF sections: "+" ".join("%s=%d"%(k,secs.get(k,0)) for k in(".text",".rodata",".data",".bss"))+"  sum=%d"%sum(secs.get(k,0) for k in(".text",".rodata",".data",".bss")))
