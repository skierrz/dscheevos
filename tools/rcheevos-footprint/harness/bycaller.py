import subprocess,collections,bisect,sys
elf,live=sys.argv[1],sys.argv[2]
syms=[]
for l in subprocess.run(["arm-none-eabi-nm","-n",elf],capture_output=True,text=True).stdout.splitlines():
    p=l.split()
    if len(p)==3 and p[1] in "tT": syms.append((int(p[0],16),p[2]))
addrs=[a for a,_ in syms]
agg=collections.Counter(); cnt=collections.Counter()
for l in open(live):
    if not l.startswith("LIVE"): continue
    _,a,n=l.split(); a=int(a,16)&~1; i=bisect.bisect_right(addrs,a)-1
    agg[syms[i][1]]+=int(n); cnt[syms[i][1]]+=1
tot=sum(agg.values())
for k,v in agg.most_common(12): print("%7d  %4d blocks  %5.1f%%  %s"%(v,cnt[k],100*v/tot,k))
print("total live",tot)
