-- fix64: the async path (ear_api 2 + file.AsyncRead): no file read on the game thread, late voices
-- start with their age, busy DLL, unusable / missing files, stops + channel replacement while
-- waiting, read-ahead (Warm64) and the remembered keys
local okc=0
local function check(c,msg) if not c then error('FAIL: '..msg,2) end okc=okc+1 end
rawset(_G,'LP',MKENT(1,Vector(0,0,0)))
local world=MKENT(0,Vector(0,0,0))
local opens=0
local open0=file.Open
file.Open=function(...) opens=opens+1 return open0(...) end
dofile(ARG_CLIENT)
local E=STTS.PhoneEar63
local M={ear_api=2,files={},voices={},loads=0,busy=0}
M.ear_load=function(k,d) M.loads=M.loads+1 if M.busy>0 then M.busy=M.busy-1 return 4 end
 if M.files[k] then return 2 end M.files[k]={st=(d:find('JUNK') and 3 or 1)} return M.files[k].st end
M.ear_state=function(k) local f=M.files[k] if not f then return 0,0,false end if f.st==1 then f.st=2 end return f.st,1.5,k:find('loop')~=nil end
M.ear_play=function(id,k,g,lp,p,loop,age) local f=M.files[k] if not f then return 2 end if f.st==3 then return 3 end M.voices[id]={k=k,g=g,lp=lp,p=p,age=age} return 1 end
M.ear_set=function(id,g,lp,p) if M.voices[id] then M.voices[id].g=g end end
M.ear_stop=function(id) M.voices[id]=nil end
local function nv() local n=0 for _ in pairs(M.voices) do n=n+1 end return n end
local function voiceOf(k) for id,v in pairs(M.voices) do if v.k==k then return v,id end end end
local function think() hooks.Think.STTS_PhoneEar64Read() end
local function emit(name,pos,ent,level,chan,flags) NEXTFRAME() hooks.EntityEmitSound.STTS_PhoneEar63({SoundName=name,Pos=pos,Entity=ent or world,SoundLevel=level or 90,Volume=1,Pitch=100,Channel=chan or 0,Flags=flags or 0}) end
for _,n in ipairs({'a','b','c','d','e','f','g','h','i','j','k','l'}) do FILES['sound/weapons/'..n..'.wav']=string.rep(n,100) end
FILES['sound/ambient/loop1.wav']=string.rep('C',100)
FILES['sound/bad/junk.wav']='JUNK'..string.rep('x',40)
SETTIME(10)
local rec={}
E.Begin(rec,M)
check(E.Active(),'active')
-- 1. a new file: no read on the game thread, the voice waits, starts with its age once read
emit('^weapons/A.wav',Vector(100,0,0))
check(nv()==0,'waits for its file')
check(opens==0,'no file.Open on the game thread')
think()
check(#ASYNCQ==1 and ASYNCQ[1].p=='sound/weapons/a.wav','one async read '..#ASYNCQ)
SETTIME(10.12) RUNASYNC() think()
local v=voiceOf('weapons/a.wav')
check(v and math.abs(v.age-0.12)<1e-6,'late voice placed by its age '..tostring(v and v.age))
check(M.loads==1,'file sent once')
-- 2. the same file again: plays at once (age 0), no new read
emit('weapons/a.wav',Vector(50,0,0))
check(nv()==2 and #ASYNCQ==0,'known file plays at once')
local n2=0 for _,vv in pairs(M.voices) do if vv.k=='weapons/a.wav' and vv.age==0 then n2=n2+1 end end
check(n2==1,'age 0 when ready')
-- 3. unusable (decoder refuses) and missing files: waiting voices dropped
emit('bad/junk.wav',Vector(10,0,0))
emit('weapons/missing.wav',Vector(10,0,0))
think() RUNASYNC() think()
check(nv()==2,'junk + missing never play')
emit('bad/junk.wav',Vector(10,0,0)) think()
check(#ASYNCQ==0,'unusable file not read again')
-- 4. DLL busy (4): the file stays and is sent the next frame
M.busy=1
emit('weapons/b.wav',Vector(10,0,0)) think() RUNASYNC() think()
check(not voiceOf('weapons/b.wav'),'busy: not yet')
think()
check(voiceOf('weapons/b.wav'),'busy: sent next frame')
-- 5. stop while waiting (Entity:StopSound) and channel replacement while waiting
local car=MKENT(50,Vector(200,0,0))
emit('ambient/loop1.wav',nil,car,80,6) think()
car:StopSound('ambient/loop1.wav')
RUNASYNC() think()
check(not voiceOf('ambient/loop1.wav'),'stopped while waiting')
emit('weapons/c.wav',nil,car,90,1) emit('weapons/d.wav',nil,car,90,1) think() RUNASYNC() think()
check(not voiceOf('weapons/c.wav') and voiceOf('weapons/d.wav'),'channel replaced while waiting')
-- 6. at most 8 reads in flight, a few started per frame
for _,n in ipairs({'e','f','g','h','i','j','k','l'}) do emit('weapons/'..n..'.wav',Vector(20,0,0),nil,90,0) end
think()
check(#ASYNCQ==6,'6 reads started per frame while filming '..#ASYNCQ)
think()
check(#ASYNCQ==8,'8 in flight '..#ASYNCQ)
SETTIME(10.5) RUNASYNC() think()
check(voiceOf('weapons/l.wav') and voiceOf('weapons/e.wav'),'all waiting voices started')
check(opens==0,'still no file.Open')
-- 7. end: the used files are remembered
E.End(rec)
SETTIME(12) RUNTIMERS()
local keys=DATA['stts_phone_ear64.txt']
check(type(keys)=='string' and keys:find('weapons/a.wav',1,true) and not keys:find('missing',1,true),'keys remembered')
check(nv()==0 and not E.Active(),'ended')
print('client64 part 1 checks passed',okc)
