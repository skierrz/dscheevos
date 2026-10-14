import sys,struct
def rd(p): return [list(map(int,l.split())) for l in open(p)]
a,b=rd(sys.argv[1]),rd(sys.argv[2])
# per-frame change counts from the trace
t=open(sys.argv[4],'rb').read(); pos=8
main,dtcm,nf,seed=struct.unpack_from('<IIII',t,pos); pos+=16+main+dtcm
nr={}
while pos<len(t):
    f,fl,_,n=struct.unpack_from('<IBBH',t,pos); pos+=8
    for i in range(n):
        ad,l=struct.unpack_from('<IH',t,pos); pos+=6+l
    nr[f]=n
def summ(sel,label):
    rows=[(ra,rb) for ra,rb in zip(a,b) if ra[0]>=2 and sel(ra[0])]
    m=len(rows)
    if not m: return
    A=sum(r[0][2] for r in rows); B=sum(r[1][2] for r in rows)
    T=sum(r[0][1] for r in rows); TB=sum(r[1][1] for r in rows)
    ctest=sum(r[1][3]-r[1][1] for r in rows); walk=sum(r[1][4] for r in rows)
    L=sum(r[0][5] for r in rows); LB=sum(r[1][5] for r in rows)
    per=sorted((rb[2]/ra[2] if ra[2] else 1) for ra,rb in rows)
    print("  %-12s %4d fr: ach conds/frame stock %6.0f cd %6.0f = %5.1f%% (median %5.1f%%, p90 %5.1f%%); lb %s; all %5.1f%%; comparison retests/fr %5.0f; snapshot walk/fr %5.0f (%4.1f%% of stock ach)"%(
      label,m,A/m,B/m,100*B/A if A else 0,100*per[m//2],100*per[int(m*.9)],("%.0f->%.0f = %.1f%%"%(L/m,LB/m,100*LB/L)) if L else "-",100*TB/T if T else 0,ctest/m,walk/m,100*walk/A if A else 0))
print(sys.argv[3])
summ(lambda f:True,'all frames')
summ(lambda f:nr.get(f,0)>0,'changing')
