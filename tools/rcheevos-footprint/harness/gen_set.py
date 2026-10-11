# Generate a synthetic but realistic DS achievement set (achievementsets v2 JSON) as a C header.
# Shapes follow common DS set authoring: pointer chains via AddAddress (I:), deltas, hit counts,
# ResetIf, OrNext/AndNext groups, Measured progress, and DTCM reads.
import json,random,sys
random.seed(int(sys.argv[3]) if len(sys.argv)>3 else 1)
n_ach=int(sys.argv[1]); n_lb=int(sys.argv[2])
def addr(): return random.randrange(0x0000100,0x03FF000)&~3
def size(): return random.choice(["0xH","0xH","0x ","0xX","0xL","0xU"])
def ptr(): return "I:0xX%06x&33554431_"%addr()          # DS pointers are 0x02xxxxxx; mask to offset
def cond(p=False):
    pre=ptr() if p else ""
    s=size(); a=random.randrange(0,0x400) if p else addr()
    op=random.choice(["=","!=",">=","<",">"])
    rhs=random.choice(["%d"%random.randrange(0,100),"d%s%06x"%(s,a)])
    return pre+"%s%06x%s%s"%(s,a,op,rhs)
def trigger(kind):
    n={"simple":random.randint(2,4),"medium":random.randint(5,10),"complex":random.randint(15,30)}[kind]
    parts=[]; measured=False
    for i in range(n):
        p=random.random()<{"simple":.2,"medium":.5,"complex":.6}[kind]
        c=cond(p)
        pre,body=(c.rsplit("_",1)[0]+"_",c.rsplit("_",1)[1]) if p else ("",c)
        r=random.random(); last=(i==n-1)
        if r<.08: body="R:"+body
        elif r<.16 and not last: body="O:"+body
        elif r<.22 and not last: body="N:"+body
        elif r<.28: body+=".%d."%random.choice([2,10,60,300])
        elif r<.31 and kind!="simple" and not measured and "d0x" not in body: body="M:"+body.split("=")[0].split(">")[0].split("<")[0].split("!")[0]+">=%d"%random.randint(5,100); measured=True
        elif r<.33: pre,body="","0xH%07x=1"%(0x1000000+random.randrange(0,0x3FFF))  # DTCM
        parts.append(pre+body)
    s="_".join(parts)
    if kind=="complex":  # alt groups
        for _ in range(random.randint(1,3)): s+="S"+"_".join(cond(random.random()<.5) for _ in range(random.randint(2,5)))
    return s
achs=[]
for i in range(n_ach):
    kind=random.choices(["simple","medium","complex"],[.4,.4,.2])[0]
    achs.append({"ID":100000+i,"Title":"Achievement number %d title"%i,"Description":"Do the thing required for achievement %d in this stage"%i,
      "Flags":3,"Points":random.choice([1,2,3,5,10,25]),"MemAddr":trigger(kind),"Author":"Author%d"%(i%5),"BadgeName":"%05d"%(200000+i),
      "Created":1600000000,"Modified":1600000000,"Type":random.choice([None,"progression","win_condition",None]),"Rarity":12.5,"RarityHardcore":8.1,
      "BadgeURL":"https://media.retroachievements.org/Badge/%05d.png"%(200000+i),"BadgeLockedURL":"https://media.retroachievements.org/Badge/%05d_lock.png"%(200000+i)})
lbs=[]
for i in range(n_lb):
    lbs.append({"ID":5000+i,"Title":"Leaderboard %d"%i,"Description":"Fastest time through course %d"%i,
      "Mem":"STA:%s::CAN:%s::SUB:%s::VAL:%s"%(trigger("medium"),trigger("simple"),trigger("simple"),"0xX%06x*1_0xH%06x*60"%(addr(),addr())),
      "Format":random.choice(["FRAMES","SCORE","MILLISECS","VALUE"]),"LowerIsBetter":True,"Hidden":False})
def rp(nlook,nent,ndisp):
    out=[]
    for l in range(nlook):
        out.append("Lookup:L%d"%l); out+= ["%d=Place name number %d"%(e,e) for e in range(nent)]; out.append("")
    out.append("Format:Score\nFormatType=VALUE\n")
    out.append("Display:")
    for d in range(ndisp): out.append("?%s?In @L%d(0xH%06x) with @Score(0xX%06x) points"%(trigger("simple"),d%max(nlook,1),addr(),addr()))
    out.append("In @L0(0xH%06x)"%addr())
    return "\n".join(out)
doc={"Success":True,"GameId":1234,"Title":"Synthetic DS Game","ConsoleId":18,"ImageIconUrl":"https://media.retroachievements.org/Images/000001.png",
     "RichPresenceGameId":1234,"RichPresencePatch":rp(int(sys.argv[4]) if len(sys.argv)>4 else 3,25,5),
     "Sets":[{"AchievementSetId":1111,"GameId":1234,"Title":None,"Type":"core","ImageIconUrl":"https://media.retroachievements.org/Images/000001.png",
              "Achievements":achs,"Leaderboards":lbs}]}
j=json.dumps(doc,separators=(",",":"))
print("/* generated: %d achievements, %d leaderboards, %d bytes JSON */"%(n_ach,n_lb,len(j)))
print("static const char patch_json[] = %s;"%json.dumps(j))
