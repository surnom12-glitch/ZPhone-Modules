-- fix73: the phone line on the server. PHONE.Line73 (the block of sv_phone.lua, run here on its own
-- with mocked helpers) + PHONE.VoiceRoute72 + the line relay of sv_phone_ear.lua.
-- python3 run.py test_line73_server.lua "" <addon>/lua/stts_phone/sv_phone_ear.lua
local okc=0
local function check(c,msg) if not c then error('FAIL: '..msg,2) end okc=okc+1 end
local function near(a,b,e) return a and math.abs(a-b)<(e or 1e-3) end
local VM=getmetatable(Vector(0,0,0))
function VM:DistToSqr(o) local d=self:Distance(o) return d*d end
VM.__sub=function(a,b) return Vector(a.x-b.x,a.y-b.y,a.z-b.z) end
VM.__tostring=function(v) return string.format('(%g %g %g)',v.x,v.y,v.z) end
local EM=FindMetaTable('Entity')
function EM:Alive() return not self.dead end
function EM:IsBot() return self.bot or false end
function EM:IsPlayer() return self.isPlayer or false end
function EM:GetParent() return self.parent end
function EM:UserID() return self.idx+100 end
function EM:TestPVS() return true end
local PLAYERS={}
player.Iterator=function() return ipairs(PLAYERS) end
player.GetHumans=function() local r={} for _,p in ipairs(PLAYERS) do if not p.bot then r[#r+1]=p end end return r end
local TIMERS={}
timer.Create=function(n,d,r,f) TIMERS[n]=f end
rawset(_G,'ErrorNoHalt',function(s) error('ErrorNoHalt: '..s) end)
-- a net mock that logs any payload (the harness's one expects the fix63 message layout)
local q
net.Start=function(n) q={name=n} end
for _,f in ipairs({'WriteBool','WriteString','WriteVector','WriteInt'}) do net[f]=function(v) q[#q+1]=v end end
net.WriteUInt=function(v,b) assert(v>=0 and v<2^b and v==math.floor(v),'uint range '..tostring(v)..' '..b) q[#q+1]=v end
net.Send=function(p) LOG[#LOG+1]='net->client '..tostring(p.name)..' '..q.name rawset(_G,'LASTMSG',q) if SENDHOOK then SENDHOOK(p,q) end end
rawset(_G,'SENDHOOK',false)
local function last() return rawget(_G,'LASTMSG') end
local function clear() rawset(_G,'LASTMSG',nil) end
local world=MKENT(0,Vector(0,0,0))
local function ply(idx,pos,name) local p=MKENT(idx,pos) p.isPlayer=true p.name=name PLAYERS[#PLAYERS+1]=p return p end
local function phone(idx,owner,pos) local w=MKENT(idx,pos or Vector(0,0,0)) w.class='weapon_stts_phone' w.owner=owner w.nw={Vol=100} if owner then owner.active=w end return w end
-- sv_phone_ear.lua first (the relay), then the line block of sv_phone.lua
dofile(ARG_SERVER)
local SE=STTS.PhoneEarSv
local src=io.open((ARG_SERVER:gsub('sv_phone_ear%.lua$','sv_phone.lua'))):read('*a')
local a=src:find('util.AddNetworkString("stts_phone_voice_gain")',1,true)
local b=src:find('\n%-%-%[%[%-+\n\tscreen state of every phone')
assert(a and b,'line block not found')
local r1=src:find('function PHONE.VoiceRoute72',1,true)
local r2=src:find('\nend\n',r1,true)
local PHONE={Calls={},ByWep={},Gagged72=function(p) return p.gagged==true end}
local CFG={PhoneSpeakerDist=280}
local function Awake(p) return IsValid(p) and p:Alive() and not (p.organism and p.organism.otrub) end
local function PhoneVol(w) return math.Clamp((w.nw.Vol or 100),0,100)/100 end
local function PhoneOf(p) return p.phone end
local function BodyOf(p) return p end
local function CharEye(p) return p.pos+Vector(0,0,64) end
local chunk=assert(loadstring('local PHONE,CFG,Awake,PhoneVol,PhoneOf,BodyOf,CharEye=...\n'..src:sub(r1,r2+4)..'\n'..src:sub(a,b)))
chunk(PHONE,CFG,Awake,PhoneVol,PhoneOf,BodyOf,CharEye)
local LINE=PHONE.Line73
check(LINE and LINE.Rebuild and TIMERS.STTS_PhoneVoiceGain,'line block loaded')
-- A holds phone wa at the origin, B holds wb 5000 units away; C stands 100 from A, D 150 from B, X far from both
local A=ply(1,Vector(0,0,0),'A') local B=ply(2,Vector(5000,0,0),'B')
local C=ply(3,Vector(100,0,0),'C') local D=ply(4,Vector(5150,0,0),'D') local X=ply(5,Vector(2500,0,0),'X')
local wa=phone(20,A) local wb=phone(21,B)
local link={state='connected',aWep=wa,bWep=wb}
PHONE.ByWep[wa]=link PHONE.ByWep[wb]=link
SETTIME(1) LINE.Rebuild()
local V=LINE.voice
-- 1. in the hand: the holders hear each other fully, the people near a phone are heard by the mic curve
check(near(V[B][A],1) and near(V[A][B],1),'holders: full voice both ways')
check(near(V[B][C],0.4),'C 100 units from the phone: 40/100 '..tostring(V[B][C]))
check(near(V[D][A],1-150/280),'D 150 from the other phone hears its speaker by distance '..tostring(V[D][A]))
check(near(V[A][D],40/150),'and is heard by its mic')
check(not (V[X] and next(V[X])) and not V[A][X],'X far from both phones: nothing')
check(not V[A][A] and not V[B][B],'nobody hears himself')
local ra,rb=PHONE.VoiceRoute72(B,C) check(ra==true and rb==false,'VoiceRoute72: on the line = 2D')
check(PHONE.VoiceRoute72(X,A)==nil,'VoiceRoute72: X not on the line = nil (normal game voice)')
-- 2. the phone on the floor (put down, A steps 54 units away from it): both directions still work
wa.owner=nil A.active=nil wa.pos=Vector(0,0,10)
LINE.Rebuild() V=LINE.voice
check(near(V[B][A],40/54),'phone on the floor: A heard by its mic '..tostring(V[B][A]))
check(near(V[A][B],1-54/280),'phone on the floor: A hears its speaker '..tostring(V[A][B]))
check(near(V[C][B],1-math.sqrt(100^2+54^2)/280),'and C next to it too')
-- 3. in the pocket (A holds another weapon): muffled mic (x0.6) and his own pocket speaker (0.3)
wa.owner=A A.active=MKENT(30,Vector(0,0,0)) wa.pos=Vector(0,0,0)
LINE.Rebuild() V=LINE.voice
check(near(V[B][A],40/64*0.6),'pocket: A muffled '..tostring(V[B][A]))
check(near(V[A][B],0.3),'pocket: A hears B quietly '..tostring(V[A][B]))
check(near(V[C][B],(1-math.sqrt(100^2+64^2)/140)*0.4),'pocket: half the speaker radius, x0.4 for C '..tostring(V[C][B]))
A.active=wa
-- 4. walls, the speaker volume, a gag, brain damage, an unconscious listener, bots
WALL=true LINE.Rebuild() V=LINE.voice
check(near(V[B][C],0.4*0.42) and near(V[D][A],(1-150/280)*0.35) and near(V[B][A],1),'walls: mic x0.42, speaker x0.35, holders unchanged')
WALL=false
wb.nw.Vol=50 LINE.Rebuild() V=LINE.voice
check(near(V[B][A],0.5) and near(V[D][A],(1-150/280)*0.5),'speaker volume 50: holder 0.5, bystander scaled')
wb.nw.Vol=0 LINE.Rebuild() V=LINE.voice
check(near(V[B][A],0.3) and not V[D],'volume 0: the holder still hears at 0.3, nobody else')
wb.nw.Vol=100
C.gagged=true LINE.Rebuild() check(not LINE.voice[B][C],'gagged: not heard') C.gagged=nil
C.organism={brain=0.5} LINE.Rebuild() check(not LINE.voice[B][C],'brain damage: not heard') C.organism=nil
D.organism={otrub=true} LINE.Rebuild() check(not LINE.voice[D],'unconscious: hears nothing') D.organism=nil
D.bot=true LINE.Rebuild() check(not LINE.voice[D],'bots get nothing') D.bot=nil
-- 5. game sounds: only for a client with the fix73 DLL, where the other phone is, muffled from a pocket
LINE.Rebuild() check(not next(LINE.ears),'no fix73 client: no ears')
FEED({true}) net.receivers.STTS_PhoneLine73(1,B)
check(B.STTS_Line73==true,'B announced fix73')
LINE.Rebuild()
local e=LINE.ears[B] and LINE.ears[B][wa.idx]
check(e and near(e.gain,1) and e.pos.z==64 and e.muffle==false,'B gets the place of wa')
A.active=MKENT(31,Vector(0,0,0)) LINE.Rebuild()
check(LINE.ears[B][wa.idx].muffle==true,'wa in a pocket: muffled') A.active=wa
FEED({true}) net.receivers.STTS_PhoneLine73(1,D) LINE.Rebuild()
check(LINE.ears[D] and near(LINE.ears[D][wa.idx].gain,1-150/280),'D (bystander with fix73) hears the sounds by his distance')
-- 6. the two phones close to each other (same room): natural 3D voices, no line sounds
wb.owner=B B.pos=Vector(300,0,0) D.pos=Vector(420,0,0)
LINE.Rebuild() V=LINE.voice
local c1,c2=PHONE.VoiceRoute72(B,A)
check(c1==true and c2==true and not (V[B] and V[B][A]),'within 360: own 3D voice')
check(not LINE.ears[B],'standing at the other phone: no line sounds')
B.pos=Vector(5000,0,0) D.pos=Vector(5150,0,0)
-- 7. the timer: voice levels + line states to the clients, the relay gets the ears
TIMERS.STTS_PhoneVoiceGain()
local toB={} for _,l in ipairs(LOG) do if l:find('^net%->client B') then toB[#toB+1]=l end end
check(#toB>=2,'B got the voice levels and the line state')
check(SE.lineL[B] and SE.lineL[B].lines[wa.idx],'relay knows B listens at wa')
-- 8. the relay: a sound near wa goes to B as kind 1 with the line id; a quiet one far away does not
local function emit(t) hooks.EntityEmitSound.STTS_PhoneEar63(t) end
SETTIME(2) NEXTFRAME() clear()
emit({SoundName='weapons/shot.wav',Pos=Vector(50,0,0),Entity=world,SoundLevel=140,Volume=1,Pitch=100,Channel=1,Flags=0})
local m=last()
check(m and m[1]==1 and m[2]==wa.idx and m[3]=='weapons/shot.wav','sound near wa relayed as line '..tostring(m and m[2]))
NEXTFRAME() clear()
emit({SoundName='weapons/step.wav',Pos=Vector(4000,0,0),Entity=world,SoundLevel=50,Volume=1,Pitch=100,Channel=0,Flags=0})
check(last()==nil,'quiet step far from wa: not relayed')
NEXTFRAME() clear()
emit({SoundName='weapons/myshot.wav',Pos=Vector(5000,0,40),Entity=world,SoundLevel=140,Volume=1,Pitch=100,Channel=1,Flags=0})
check(last()==nil,'B\'s own shot (at B and D): not echoed back through the line')
local own=MKENT(41,Vector(5010,0,0))
NEXTFRAME() clear() own:StopSound('weapons/myloop.wav')
check(last()==nil,'a stop 5000 units from the other phone: nothing to stop on the line')
local npc=MKENT(40,Vector(60,0,0))
NEXTFRAME() clear() npc:StopSound('npc/loop.wav')
local lm=last() check(lm and lm[1]==1 and lm[5]==40 and lm[11]==4,'StopSound near wa relayed with the entity index')
-- rate limit 40/s per listener (B and D both listen: count B only)
local nB=0
SENDHOOK=function(p,m) if p==B and m.name=='STTS_PhoneLine73' and m[1]==1 then nB=nB+1 end end
SETTIME(3)
for i=1,100 do NEXTFRAME() emit({SoundName='weapons/r'..i..'.wav',Pos=Vector(10,0,0),Entity=world,SoundLevel=90,Volume=1,Pitch=100,Channel=0,Flags=0}) end
check(nB>=35 and nB<=41,'line rate limit '..nB)
-- 9. the call ends: no ears, nothing relayed, a last empty state
link.state='ended' PHONE.ByWep={}
TIMERS.STTS_PhoneVoiceGain()
check(not next(SE.lineL),'relay cleared')
SETTIME(5) NEXTFRAME() clear()
emit({SoundName='weapons/shot.wav',Pos=Vector(50,0,0),Entity=world,SoundLevel=140,Volume=1,Pitch=100,Channel=1,Flags=0})
check(last()==nil,'nothing after the call')
-- 10. 911: the caller's phone <-> the operator's headset; the operator (spectator) with fix73 hears the scene
local O=ply(6,Vector(-9000,0,0),'O') O.dead=true
A.phone=wa PHONE.Calls[A]={state='connected',op=O}
FEED({true}) net.receivers.STTS_PhoneLine73(1,O)
LINE.Rebuild() V=LINE.voice
check(near(V[O][A],1) and near(V[O][C],0.4),'operator hears the caller and the people near his phone')
check(near(V[A][O],1) and near(V[C][O],1-100/280),'the caller hears the operator, C next to him through the speaker')
check(LINE.ears[O] and LINE.ears[O][wa.idx],'operator gets the sounds around the caller')
print('line73 server checks passed',okc)
