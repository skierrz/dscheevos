import subprocess,collections,bisect,sys
elf,tr=sys.argv[1],sys.argv[2]
syms=[]
for l in subprocess.run(["arm-none-eabi-nm","-n","-S",elf],capture_output=True,text=True).stdout.splitlines():
    p=l.split()
    if len(p)==4 and p[2] in "tTwW": syms.append((int(p[0],16),p[3]))
addrs=[a for a,_ in syms]
agg=collections.Counter()
for l in open(tr):
    if not l.startswith("PC"): continue
    _,pc,n=l.split(); a=int(pc,16); i=bisect.bisect_right(addrs,a)-1; agg[syms[i][1]]+=int(n)
tot=sum(agg.values())
print("total instructions in measured frames:",tot)
for k,v in agg.most_common(int(sys.argv[3]) if len(sys.argv)>3 else 20): print("%10d %5.1f%%  %s"%(v,100*v/tot,k))
