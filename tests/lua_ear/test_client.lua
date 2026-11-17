local okc=0
local function check(c,msg) if not c then error('FAIL: '..msg,2) end okc=okc+1 end
rawset(_G,'LP',MKENT(1,Vector(0,0,0)))
local world=MKENT(0,Vector(0,0,0))
dofile(ARG_CLIENT)
local E=STTS.PhoneEar63
check(E and E.GainAt,'module loaded')
-- gain curve sanity (Source): level 75 at 36 units ~ 1 region; far ~ small
local g1,g2,g3=E.GainAt(75,36),E.GainAt(75,1000),E.GainAt(140,10000)
print('gain 75@36',g1,'75@1000',g2,'140@10000',g3)
check(g1>0.8 and g2<0.25 and g2>0 and g3>0.05,'gain curve')
-- fake DLL
local M={files={},voices={},sets=0}
M.ear_load=function(k,d) M.files[k]={st=(d:find('JUNK') and 3 or 1)} return M.files[k].st end
M.ear_state=function(k) local f=M.files[k] if not f then return 0,0,false end if f.st==1 then f.st=2 end return f.st, 1.5, k:find('loop')~=nil end
M.ear_play=function(id,k,g,lp,p,loop) local f=M.files[k] if not f then return 2 end if f.st==3 then return 3 end M.voices[id]={k=k,g=g,lp=lp,p=p} return 1 end
M.ear_set=function(id,g,lp,p) M.sets=M.sets+1 if M.voices[id] then M.voices[id].g=g M.voices[id].lp=lp end end
M.ear_stop=function(id) M.voices[id]=nil end
local function nv() local n=0 for _ in pairs(M.voices) do n=n+1 end return n end
FILES['sound/weapons/a.wav']=string.rep('A',100)
FILES['sound/weapons/b.wav']=string.rep('B',100)
FILES['sound/ambient/loop1.wav']=string.rep('C',100)
FILES['sound/bad/junk.wav']='JUNK'..string.rep('x',40)
SETTIME(1)
local rec={wep=nil}
E.Begin(rec,M)
check(E.Active(),'active')
check(LOG[#LOG]=='net->server true','told server')
-- a shot near the phone
NEXTFRAME()
hooks.EntityEmitSound.STTS_PhoneEar63({SoundName='^weapons/A.wav',Pos=Vector(100,0,0),Entity=world,SoundLevel=140,Volume=1,Pitch=100,Channel=1,Flags=0})
check(nv()==1,'one voice')
local v=M.voices[1]
check(v.k=='weapons/a.wav','key normalized '..tostring(v and v.k))
check(v.lp==20000,'open lp near')
-- same frame duplicate via sound.Play wrapper
sound.Play('^weapons/A.wav',Vector(100,0,0),140,100,1)
check(nv()==1,'dedup same frame')
check(LOG[#LOG]=='origPlay ^weapons/A.wav','orig play called')
-- the server repeats it 0.1 s later: dropped
SETTIME(1.1) NEXTFRAME()
FEED({'^weapons/A.wav',Vector(100,0,0),0,false,140,255,100,1,0})
net.receivers.STTS_PhoneEar63()
check(nv()==1,'cross-origin dedup')
-- behind a wall, far: muffled
WALL=true NEXTFRAME()
hooks.EntityEmitSound.STTS_PhoneEar63({SoundName='weapons/b.wav',Pos=Vector(3000,0,0),Entity=world,SoundLevel=140,Volume=1,Pitch=100,Channel=0,Flags=0})
check(nv()==2,'far voice')
check(M.voices[2].lp<=1300,'wall lowpass '..M.voices[2].lp)
WALL=false
-- quiet far sound culled
NEXTFRAME()
hooks.EntityEmitSound.STTS_PhoneEar63({SoundName='weapons/b.wav',Pos=Vector(8000,0,0),Entity=world,SoundLevel=60,Volume=0.5,Pitch=100,Channel=0,Flags=0})
check(nv()==2,'culled quiet far')
-- 2D sound (level 0) ignored
hooks.EntityEmitSound.STTS_PhoneEar63({SoundName='weapons/b.wav',Entity=world,SoundLevel=0,Volume=1,Pitch=100,Channel=0,Flags=0})
check(nv()==2,'2d ignored')
-- junk file refused
NEXTFRAME()
hooks.EntityEmitSound.STTS_PhoneEar63({SoundName='bad/junk.wav',Pos=Vector(10,0,0),Entity=world,SoundLevel=75,Volume=1,Pitch=100,Channel=0,Flags=0})
check(nv()==2,'junk refused')
-- looping sound on a moving entity (server relayed, follow), then entity moves
local car=MKENT(50,Vector(200,0,0))
SETTIME(1.5) NEXTFRAME()
FEED({'ambient/loop1.wav',Vector(200,0,0),50,true,80,255,100,6,0})
net.receivers.STTS_PhoneEar63()
check(nv()==3,'loop voice')
local lid
for id,vv in pairs(M.voices) do if vv.k=='ambient/loop1.wav' then lid=id end end
local g0=M.voices[lid].g
car.pos=Vector(2000,0,0)
SETTIME(1.6) NEXTFRAME()
E.Step(rec,Vector(0,0,0))
check(M.voices[lid].g<g0,'follows entity: quieter '..g0..' -> '..M.voices[lid].g)
-- the one-shots end after their length (1.5 s)
SETTIME(3.5) NEXTFRAME()
E.Step(rec,Vector(0,0,0))
check(nv()==1 and M.voices[lid],'one-shots over, loop stays '..nv())
-- server says StopSound with a sound script name on another entity: nothing
FEED({'Script.X',Vector(0,0,0),50,true,75,255,100,0,4})
net.receivers.STTS_PhoneEar63()
check(nv()==1,'script stop of other files keeps loop')
-- stop the loop by name via Entity:StopSound wrapper
car:StopSound('ambient/loop1.wav')
check(nv()==0,'StopSound wrapper stops')
check(LOG[#LOG]=='origStop ambient/loop1.wav','orig stop called')
-- CreateSound + Stop
NEXTFRAME() SETTIME(4)
local p=CreateSound(car,'ambient/loop1.wav')
hooks.EntityEmitSound.STTS_PhoneEar63({SoundName='ambient/loop1.wav',Entity=car,SoundLevel=80,Volume=1,Pitch=100,Channel=6,Flags=0})
check(nv()==1,'patch voice')
p:FadeOut(0.5)
SETTIME(4.6) RUNTIMERS()
check(nv()==0,'fade out stops later')
-- channel replacement: same entity, same channel
NEXTFRAME()
hooks.EntityEmitSound.STTS_PhoneEar63({SoundName='weapons/a.wav',Entity=car,SoundLevel=90,Volume=1,Pitch=100,Channel=1,Flags=0})
NEXTFRAME()
hooks.EntityEmitSound.STTS_PhoneEar63({SoundName='weapons/b.wav',Entity=car,SoundLevel=90,Volume=1,Pitch=120,Channel=1,Flags=0})
check(nv()==1,'channel replaced')
-- change pitch flag
NEXTFRAME()
hooks.EntityEmitSound.STTS_PhoneEar63({SoundName='weapons/b.wav',Entity=car,SoundLevel=90,Volume=1,Pitch=80,Channel=1,Flags=2})
check(nv()==1,'change does not add')
-- dormant entity follows not
car.dormant=true
NEXTFRAME() SETTIME(5)
FEED({'weapons/a.wav',Vector(300,0,0),50,true,90,255,100,0,0})
net.receivers.STTS_PhoneEar63()
check(nv()==2,'dormant relay plays at server pos')
-- pocket step (pos nil): owner position
local owner=MKENT(2,Vector(500,0,0))
local wep=MKENT(3,Vector(500,0,0)) wep.owner=owner
rec.wep=wep
E.Step(rec,nil)
-- RenderScene view
hooks.RenderScene.STTS_PhoneEarView63(Vector(1,2,3))
check(E.view and E.view.x==1,'view stored')
-- 64 voice cap
for i=1,80 do NEXTFRAME() hooks.EntityEmitSound.STTS_PhoneEar63({SoundName='weapons/a.wav',Pos=Vector(i*10,0,0),Entity=world,SoundLevel=90,Volume=1,Pitch=100,Channel=0,Flags=0}) end
check(nv()<=64,'voice cap '..nv())
-- server without fix63: End does not try net
NETOK=false
E.End(rec)
check(nv()==0,'all stopped on end')
check(not E.Active(),'inactive')
check(LOG[#LOG]~='net->server false','no net without server support')
-- sounds after end ignored
hooks.EntityEmitSound.STTS_PhoneEar63({SoundName='weapons/a.wav',Pos=Vector(1,0,0),Entity=world,SoundLevel=90,Volume=1,Pitch=100,Channel=0,Flags=0})
check(nv()==0,'ignored after end')
print('client checks passed',okc,'sets',M.sets)
