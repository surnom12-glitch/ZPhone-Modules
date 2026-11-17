-- fix73: the phone line on the client (cl_stts_phone_line73.lua + the line instance of
-- cl_stts_phone_ear63.lua): capability announce, line state, sounds shaped at the OTHER phone,
-- pocket muffle, speaker gain, stops, entities this client never saw, the recording path untouched
-- at the same time, master gain (volume, focus), close on end / stale state / lost device.
-- python3 run.py test_line73.lua <addon>/lua/autorun/client/cl_stts_phone_ear63.lua
local okc=0
local function check(c,msg) if not c then error('FAIL: '..msg,2) end okc=okc+1 end
rawset(_G,'LP',MKENT(1,Vector(0,0,0)))
local world=MKENT(0,Vector(0,0,0))
-- the GMod bits the line file needs (not in harness.lua)
local CV={}
local function cvar(name,def) local c={v=tonumber(def)} function c:GetBool() return self.v~=0 end function c:GetInt() return math.floor(self.v) end function c:GetFloat() return self.v end CV[name]=c return c end
rawset(_G,'CreateClientConVar',function(name,def) return cvar(name,def) end)
cvar('volume','0.5') cvar('snd_mute_losefocus','1')
rawset(_G,'GetConVar',function(n) return CV[n] end)
rawset(_G,'cvars',{AddChangeCallback=function() end})
local TIMERS={}
timer.Create=function(n,d,r,f) TIMERS[n]=f end
local CMDS={}
rawset(_G,'concommand',{Add=function(n,f) CMDS[n]=f end})
local FOCUS=true
rawset(_G,'system',{HasFocus=function() return FOCUS end})
rawset(_G,'Color',function() return {} end)
rawset(_G,'MsgC',function() end)
-- the native module: ear_* (the clip) and line_* (the call) on one shared file bank
local M={ear_api=2,line_api=1,files={},ear={},line={},opens=0,closes=0,alive=true,gains={}}
M.ear_load=function(k,d) if M.files[k] then return 2 end M.files[k]={st=1} return 1 end
M.ear_state=function(k) local f=M.files[k] if not f then return 0,0,false end if f.st==1 then f.st=2 end return f.st,1.5,k:find('loop')~=nil end
M.ear_play=function(id,k,g,lp,p,loop,age) local f=M.files[k] if not f then return 2 end M.ear[id]={k=k,g=g,lp=lp} return 1 end
M.ear_set=function(id,g,lp) if M.ear[id] then M.ear[id].g=g M.ear[id].lp=lp end end
M.ear_stop=function(id) M.ear[id]=nil end
M.line_open=function() M.opens=M.opens+1 M.alive=true return 1 end
M.line_close=function() M.closes=M.closes+1 M.line={} end
M.line_gain=function(master,muf,band) M.gains[#M.gains+1]={master,muf,band} return M.alive end
M.line_play=function(id,k,g,lp,p,loop,age) local f=M.files[k] if not f then return 2 end if f.st==3 then return 3 end M.line[id]={k=k,g=g,lp=lp,p=p,age=age} return 1 end
M.line_set=function(id,g,lp,p) if M.line[id] then M.line[id].g=g M.line[id].lp=lp end end
M.line_stop=function(id) if id==nil then M.line={} M.stopAll=true else M.line[id]=nil end end
M.line_poll=function() return {open=true,state=1,rate=48000,channels=2,queuedMs=50,underruns=0,reopened=0,voices=0,played=0,peak=0,seconds=0,error=''} end
local loaded=M
STTS.PhoneRecorder31={Native64=function() return loaded end,Probe73=function() return loaded end}
dofile(ARG_CLIENT)
dofile((ARG_CLIENT:gsub('cl_stts_phone_ear63%.lua$','cl_stts_phone_line73.lua')))
local E,LN=STTS.PhoneEar63,STTS.PhoneLine73
local function n(t) local c=0 for _ in pairs(t) do c=c+1 end return c end
local function lineOf(k) for id,v in pairs(M.line) do if v.k==k then return v,id end end end
local function think() NEXTFRAME() if hooks.Think.STTS_PhoneEar64Read then hooks.Think.STTS_PhoneEar64Read() end hooks.Think.STTS_PhoneLine73() end
local function told() for i=#LOG,1,-1 do local l=LOG[i] if l:find('^net%->server') then return l end end end
local function state(list) local q={0,#list} for _,l in ipairs(list) do q[#q+1]=l[1] q[#q+1]=l[2] q[#q+1]=l[3] or false q[#q+1]=l[4] or 255 end FEED(q) net.receivers.STTS_PhoneLine73() end
local function snd(id,name,pos,ei,level,flags,chan,follow) FEED({1,id,name,pos,ei or 0,follow or false,level or 90,255,100,chan or 0,flags or 0}) net.receivers.STTS_PhoneLine73() end
for _,f in ipairs({'a','b','c','d','loop1'}) do FILES['sound/weapons/'..f..'.wav']=string.rep('x',100) end
SETTIME(10)
-- 1. announce: the server learns that this client can play the line
TIMERS.STTS_PhoneLine73Tell()
check(told()=='net->server true','capability announced: '..tostring(told()))
local before=#LOG TIMERS.STTS_PhoneLine73Tell()
check(#LOG==before,'not announced again every 3 s')
-- 2. a line: the other phone 5000 units away; the native line opens (once)
state({{10,Vector(5000,0,64)}})
think()
check(M.opens==1 and E.LineActive(),'line opened')
think() check(M.opens==1,'opened once')
-- 3. a sound near the other phone: read async, played on the line (not on the clip ear)
snd(10,'^weapons/A.wav',Vector(5010,0,64),0,90)
think() RUNASYNC() think()
local v=lineOf('weapons/a.wav')
check(v and v.g>0.9 and v.lp==20000 and n(M.ear)==0,'near the other phone: loud and open on the line '..tostring(v and v.g))
-- 4. the same sound far from the other phone (but where YOU stand): quiet / not played
snd(10,'weapons/b.wav',Vector(0,0,0),0,50)
think() RUNASYNC() think()
check(not lineOf('weapons/b.wav'),'quiet far from the other phone: not played')
-- 5. an unknown line id: ignored
snd(11,'weapons/a.wav',Vector(5010,0,64),0,90) think()
check(n(M.line)==1,'unknown line ignored')
-- 6. speaker gain and pocket muffle of the line re-shape the playing voice
SETTIME(10.1) state({{10,Vector(5000,0,64),true,128}}) think()
v=lineOf('weapons/a.wav')
check(v and v.lp<=900 and v.g<0.4 and v.g>0.3,'pocket: muffled and quieter '..tostring(v and v.g)..' '..tostring(v and v.lp))
SETTIME(10.2) state({{10,Vector(5000,0,64),false,255}}) think()
-- 7. the recording at the same time (filming during a call): the clip ear is not mixed up with the line
local rec={}
E.Begin(rec,M)
hooks.EntityEmitSound.STTS_PhoneEar63({SoundName='weapons/c.wav',Pos=Vector(10,0,0),Entity=world,SoundLevel=80,Volume=1,Pitch=100,Channel=0,Flags=0})
think() RUNASYNC() think()
check(n(M.ear)==1 and M.ear[1] and M.ear[1].k=='weapons/c.wav' and n(M.line)==1,'clip ear and line separate')
E.Step(rec,Vector(0,0,0))
E.End(rec)
check(n(M.ear)==0 and n(M.line)==1 and E.LineActive(),'ending the clip leaves the line')
-- 8. a loop on an entity this client never saw (index 77): stop by the server's index
snd(10,'weapons/loop1.wav',Vector(5020,0,64),77,80,0,6)
think() RUNASYNC() think()
local lv,lid=lineOf('weapons/loop1.wav')
check(lv,'loop of an unseen entity plays')
snd(10,'weapons/loop1.wav',Vector(5020,0,64),77,80,4) think()
check(not lineOf('weapons/loop1.wav'),'stopped by the server index')
-- channel replacement on the unseen entity
snd(10,'weapons/c.wav',Vector(5020,0,64),77,90,0,1) snd(10,'weapons/d.wav',Vector(5020,0,64),77,90,0,1)
think() RUNASYNC() think()
check(not lineOf('weapons/c.wav') and lineOf('weapons/d.wav'),'channel replaced on the unseen entity')
-- 9. master: zhphone_line_gain x volume; 0 without focus (snd_mute_losefocus)
local g=M.gains[#M.gains]
check(math.abs(g[1]-0.5)<1e-9 and g[2]==0 and g[3]==1,'master = gain x volume, band 1')
FOCUS=false think() check(M.gains[#M.gains][1]==0,'no focus: silent') FOCUS=true
CV.zhphone_line_band.v=2 think() check(M.gains[#M.gains][3]==2,'band setting passed')
-- 10. the native line ended (device lost / watchdog): closed, opened again a second later
M.alive=false think()
check(not E.LineActive() and M.closes==1,'line ended -> closed')
M.alive=true SETTIME(10.5) state({{10,Vector(5000,0,64)}}) think()
check(M.opens==1,'not reopened at once')
SETTIME(11.3) state({{10,Vector(5000,0,64)}}) think()
check(M.opens==2 and E.LineActive(),'reopened after 1 s')
-- 11. the call is over (empty state): every voice stopped, the line closed
snd(10,'weapons/a.wav',Vector(5010,0,64),0,90) think()
check(n(M.line)>=1,'playing again')
SETTIME(11.4) state({}) think()
check(not E.LineActive() and M.closes==2 and n(M.line)==0,'call over: closed, voices stopped')
-- 12. a stale state (the server went quiet) also closes
SETTIME(12) state({{10,Vector(5000,0,64)}}) think() check(E.LineActive(),'open again')
SETTIME(13.6) think()
check(not E.LineActive(),'stale for 1.5 s: closed')
-- 13. zhphone_line_ear 0: never opens, the server is told
CV.zhphone_line_ear.v=0
SETTIME(14) state({{10,Vector(5000,0,64)}}) think()
check(not E.LineActive(),'line ear off: not opened')
TIMERS.STTS_PhoneLine73Tell()
check(told()=='net->server false','told the server: off')
CV.zhphone_line_ear.v=1
-- 14. a DLL without the line (fix70): never announced
loaded={ear_api=2,ear_load=M.ear_load,ear_state=M.ear_state,ear_play=M.ear_play,ear_set=M.ear_set,ear_stop=M.ear_stop}
TIMERS.STTS_PhoneLine73Tell()
check(told()=='net->server false','fix70 DLL: not capable')
SETTIME(15) state({{10,Vector(5000,0,64)}}) think()
check(not E.LineActive(),'fix70 DLL: no line')
loaded=M
-- 15. the stats command runs
CMDS.zhphone_line_stats()
print('line73 client checks passed',okc)
