-- fix64: read-ahead in a fresh game session (Warm64 with remembered keys), then a clip without waits
local okc=0
local function check(c,msg) if not c then error('FAIL: '..msg,2) end okc=okc+1 end
rawset(_G,'LP',MKENT(1,Vector(0,0,0)))
local world=MKENT(0,Vector(0,0,0))
DATA['stts_phone_ear64.txt']='weapons/a.wav\nweapons/b.wav\n../evil.wav\nweapons/missing.wav'
dofile(ARG_CLIENT)
local E=STTS.PhoneEar63
local M={ear_api=2,files={},voices={}}
M.ear_load=function(k,d) if M.files[k] then return 2 end M.files[k]={st=1} return 1 end
M.ear_state=function(k) local f=M.files[k] if not f then return 0,0,false end if f.st==1 then f.st=2 end return f.st,1.5,false end
M.ear_play=function(id,k,g,lp,p,loop,age) local f=M.files[k] if not f then return 2 end M.voices[id]={k=k,age=age} return 1 end
M.ear_set=function() end M.ear_stop=function(id) M.voices[id]=nil end
FILES['sound/weapons/a.wav']=string.rep('a',100) FILES['sound/weapons/b.wav']=string.rep('b',100)
-- a fix63 DLL (ear_api 1): no read-ahead
E.Warm64({ear_load=M.ear_load,ear_play=M.ear_play})
hooks.Think.STTS_PhoneEar64Read()
check(#ASYNCQ==0,'no read-ahead for an old DLL')
SETTIME(1) E.Warm64(M)
hooks.Think.STTS_PhoneEar64Read() hooks.Think.STTS_PhoneEar64Read()
check(#ASYNCQ==3,'remembered keys read ahead (bad key skipped) '..#ASYNCQ)
SETTIME(1.1) RUNASYNC() hooks.Think.STTS_PhoneEar64Read()
check(M.files['weapons/a.wav'] and M.files['weapons/b.wav'],'in the DLL before recording')
E.Begin({},M)
NEXTFRAME() hooks.EntityEmitSound.STTS_PhoneEar63({SoundName='weapons/a.wav',Pos=Vector(30,0,0),Entity=world,SoundLevel=90,Volume=1,Pitch=100,Channel=0,Flags=0})
local played=false for _,v in pairs(M.voices) do if v.k=='weapons/a.wav' and v.age==0 then played=true end end
check(played,'read-ahead file plays in the first frame')
check(#ASYNCQ==0,'no new read')
print('client64 part 2 checks passed',okc)
