import json, time, importlib, os
os.environ["DRY_RUN"]="1"
import watcher as W

class N:
    def __init__(s): s.out=[]
    def send(s,t,urgent=False): s.out.append(t)
def st(t,mains="on"): return json.dumps({"mode":"realtime","temp_f":t,"mains":mains,"fw":"x"})
from datetime import datetime
T0=datetime(2026,10,13,10,0,0,tzinfo=W.TZ).timestamp()

def check(name,cond):
    print(("PASS" if cond else "FAIL"),name); assert cond

# cold
n=N(); w=W.Watcher(n,T0)
w.on_message("x/status",st(50),False,T0+60); check("no alert at 50",n.out==[])
w.on_message("x/status",st(44),False,T0+120); check("one low reading: no alert",n.out==[])
w.on_message("x/status",st(44),False,T0+240); check("2nd low reading alerts",len(n.out)==1 and n.out[0].startswith("COLD ALERT"))
w.on_message("x/status",st(43),False,T0+360); check("no repeat before 6h",len(n.out)==1)
w.on_message("x/status",st(43),False,T0+360+6*3600); check("repeat after 6h",len(n.out)==2 and n.out[1].startswith("STILL COLD"))
w.on_message("x/status",st(46),False,T0+30000); check("46F is not clear yet",len(n.out)==2)
w.on_message("x/status",st(47.5),False,T0+30100); check("all clear at 47+",n.out[-1].startswith("ALL CLEAR"))
# retained status never triggers
n=N(); w=W.Watcher(n,T0)
for i in range(3): w.on_message("x/status",st(30),True,T0+i)
check("retained cold status ignored",n.out==[])

# power: 6 minute blip -> restore text only
n=N(); w=W.Watcher(n,T0)
w.on_message("x/power",json.dumps({"event":"outage"}),False,T0)
w.tick(T0+300); check("no outage text before 20 min",n.out==[])
w.on_message("x/power",json.dumps({"event":"restore","outage_s":360}),False,T0+360)
check("6-min outage -> restore summary",len(n.out)==1 and "POWER RESTORED: power was out for 6 minutes (10:00 AM-10:06 AM Central)" in n.out[0])
# 2-minute blip -> silent
n=N(); w=W.Watcher(n,T0)
w.on_message("x/power",json.dumps({"event":"outage"}),False,T0)
w.on_message("x/power",json.dumps({"event":"restore","outage_s":120}),False,T0+120)
check("2-min blip silent",n.out==[])
# long outage
n=N(); w=W.Watcher(n,T0)
w.on_message("x/status",st(66),False,T0-60)
w.on_message("x/power",json.dumps({"event":"outage"}),False,T0)
w.on_message("x/status",st(65,"off"),False,T0+600)
w.tick(T0+19*60); check("nothing at 19 min",n.out==[])
w.on_message("x/status",st(64,"off"),False,T0+20*60); w.tick(T0+20*60+1)
check("outage text at 20 min",len(n.out)==1 and "POWER OUT: power went out at 10:00 AM Central and has not been restored. House is 64F." in n.out[0])
w.tick(T0+30*60); check("outage text once",len(n.out)==1)
w.on_message("x/power",json.dumps({"event":"restore","outage_s":4320}),False,T0+72*60)
check("restore after long outage",n.out[-1].startswith("POWER RESTORED: power was out for 1 hr 12 min (10:00 AM-11:12 AM Central)"))

# offline
n=N(); w=W.Watcher(n,T0)
w.on_message("x/status",st(60),False,T0+10)
w.tick(T0+10+29*60); check("no offline at 29 min",n.out==[])
w.tick(T0+10+31*60); check("offline at 31 min",len(n.out)==1 and n.out[0].startswith("HUB OFFLINE"))
w.tick(T0+10+60*60); check("offline not repeated at 1h",len(n.out)==1)
w.on_message("x/status",st(61),False,T0+10+90*60); check("back online",n.out[-1].startswith("HUB BACK ONLINE"))
# pong counts as alive
n=N(); w=W.Watcher(n,T0)
w.on_message("x/status",json.dumps({"event":"pong"}),False,T0+25*60); w.tick(T0+50*60)
check("pong resets offline timer",n.out==[])
print("ALL OK")
