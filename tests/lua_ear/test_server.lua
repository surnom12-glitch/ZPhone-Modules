local okc=0
local function check(c,msg) if not c then error('FAIL: '..msg,2) end okc=okc+1 end
local world=MKENT(0,Vector(0,0,0))
local ply=MKENT(1,Vector(0,0,0)) ply.name='P1'
local other=MKENT(2,Vector(0,0,0)) other.name='P2'
local phone=MKENT(10,Vector(5000,0,0)) phone.class='weapon_stts_phone' phone.nw={CamMode=2,RecBy=ply}
dofile(ARG_SERVER)
check(LOG[1]=='addnet STTS_PhoneEar63','net string')
local function emit(t) hooks.EntityEmitSound.STTS_PhoneEar63(t) end
local function sent() local n=0 for _,l in ipairs(LOG) do if l:find('^net%->client') then n=n+1 end end return n end
SETTIME(1) NEXTFRAME()
emit({SoundName='weapons/a.wav',Pos=Vector(5100,0,0),Entity=world,SoundLevel=90,Volume=1,Pitch=100,Channel=1,Flags=0})
check(sent()==0,'nobody listens: nothing sent')
FEED({true}) net.receivers.STTS_PhoneEar63(1,ply)
NEXTFRAME()
emit({SoundName='weapons/a.wav',Pos=Vector(5100,0,0),Entity=world,SoundLevel=90,Volume=1,Pitch=100,Channel=1,Flags=0})
check(sent()==1,'near phone: sent '..sent())
-- far from phone but near player: not sent (phone is far)
NEXTFRAME()
emit({SoundName='weapons/b.wav',Pos=Vector(10,0,0),Entity=world,SoundLevel=50,Volume=1,Pitch=100,Channel=1,Flags=0})
check(sent()==1,'far from phone not sent')
-- loud far gunshot reaches
NEXTFRAME()
emit({SoundName='weapons/b.wav',Pos=Vector(0,0,0),Entity=world,SoundLevel=140,Volume=1,Pitch=100,Channel=1,Flags=0})
check(sent()==2,'loud far sent')
-- same tick twice (hook + sound.Play): once
NEXTFRAME()
emit({SoundName='weapons/c.wav',Pos=Vector(5000,10,0),Entity=world,SoundLevel=75,Volume=1,Pitch=100,Channel=0,Flags=0})
sound.Play('weapons/c.wav',Vector(5000,10,0),75,100,1)
check(sent()==3,'tick dedup '..sent())
-- entity follow + stop + chan 136
local npc=MKENT(20,Vector(5050,0,0))
NEXTFRAME()
emit({SoundName='npc/x.wav',Entity=npc,SoundLevel=75,Volume=1,Pitch=100,Channel=136,Flags=0})
check(sent()==4 and LASTMSG[4]==true and LASTMSG[8]==136,'follow + chan 136')
NEXTFRAME()
npc:StopSound('npc/x.wav')
check(sent()==5 and LASTMSG[9]==4,'stop relayed')
-- CreateSound patch stop
NEXTFRAME()
local p=CreateSound(npc,'npc/loop.wav')
p:Stop()
check(sent()==6 and LASTMSG[1]=='npc/loop.wav' and LASTMSG[9]==4,'patch stop relayed')
-- rate limit: 100 sounds in one moment -> ~40
for i=1,100 do NEXTFRAME() emit({SoundName='weapons/r'..i..'.wav',Pos=Vector(5000,0,0),Entity=world,SoundLevel=75,Volume=1,Pitch=100,Channel=0,Flags=0}) end
local n=sent() print('after burst',n)
check(n<=6+40 and n>=6+30,'rate limit '..n)
-- phone no longer recording: nothing
phone.nw.CamMode=1
SETTIME(3) NEXTFRAME()
emit({SoundName='weapons/z.wav',Pos=Vector(5000,0,0),Entity=world,SoundLevel=75,Volume=1,Pitch=100,Channel=0,Flags=0})
check(sent()==n,'not recording: nothing')
-- phone in owner's hands (held): eye pos used
phone.nw.CamMode=2 phone.owner=other other.pos=Vector(-3000,0,0) other.active=phone
SETTIME(4) NEXTFRAME()
emit({SoundName='weapons/q.wav',Pos=Vector(-3000,30,0),Entity=world,SoundLevel=70,Volume=1,Pitch=100,Channel=0,Flags=0})
check(sent()==n+1,'phone held by other: near him')
-- opt out
SETTIME(5)
FEED({false}) net.receivers.STTS_PhoneEar63(1,ply)
NEXTFRAME()
emit({SoundName='weapons/q2.wav',Pos=Vector(-3000,30,0),Entity=world,SoundLevel=70,Volume=1,Pitch=100,Channel=0,Flags=0})
check(sent()==n+1,'opted out')
hooks.PlayerDisconnected.STTS_PhoneEar63(ply)
print('server checks passed',okc)
