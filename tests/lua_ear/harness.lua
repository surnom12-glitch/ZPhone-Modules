-- minimal GMod mocks for cl_stts_phone_ear63.lua / sv_phone_ear.lua / recorder bits
local log={}
_G.LOG=log
local function L(...) log[#log+1]=table.concat({...},' ') end
_G.L=L
local VM={}
VM.__index=VM
function _G.Vector(x,y,z) if type(x)=='table' then return setmetatable({x=x.x,y=x.y,z=x.z},VM) end return setmetatable({x=x or 0,y=y or 0,z=z or 0},VM) end
function VM:Distance(o) local dx,dy,dz=self.x-o.x,self.y-o.y,self.z-o.z return math.sqrt(dx*dx+dy*dy+dz*dz) end
VM.__add=function(a,b) return Vector(a.x+b.x,a.y+b.y,a.z+b.z) end
function _G.isvector(v) return getmetatable(v)==VM end
_G.MASK_SOLID_BRUSHONLY=16395
_G.HOOK_MONITOR_HIGH=-2
_G.CLIENT=(ARG_SERVER=="");_G.SERVER=(ARG_SERVER~="")
local T=0
_G.SETTIME=function(t) T=t end
function _G.RealTime() return T end
function _G.CurTime() return T end
local frame=0
_G.NEXTFRAME=function() frame=frame+1 end
function _G.FrameNumber() return frame end
_G.bit={band=function(a,b) local r,p=0,1 while a>0 and b>0 do if a%2==1 and b%2==1 then r=r+p end a=math.floor(a/2) b=math.floor(b/2) p=p*2 end return r end}
function math.Clamp(v,a,b) return math.min(math.max(v,a),b) end
_G.hooks={}
_G.hook={Add=function(ev,name,fn) hooks[ev]=hooks[ev] or {} hooks[ev][name]=fn end, Run=function() end}
_G.WALL=false
_G.util={TraceLine=function(t) t.output.Hit=WALL t.output.Fraction=WALL and 0.5 or 1 t.output.StartSolid=false return t.output end,
 NetworkStringToID=function(s) return NETOK and 1 or 0 end, AddNetworkString=function(s) L('addnet',s) end}
_G.NETOK=true
local netq={}
_G.net={Start=function(n) netq={name=n} end, WriteBool=function(b) netq[#netq+1]=b end, SendToServer=function() L('net->server',tostring(netq[1])) end,
 WriteString=function(v) netq[#netq+1]=v end, WriteVector=function(v) netq[#netq+1]=v end, WriteUInt=function(v,b) assert(v>=0 and v<2^b,'uint range '..v..' '..b) netq[#netq+1]=v end,
 WriteInt=function(v,b) assert(v>=-(2^(b-1)) and v<2^(b-1),'int range') netq[#netq+1]=v end, Send=function(p) L('net->client',p.name,netq[1],tostring(netq[4]),netq[5],netq[9]) _G.LASTMSG=netq end,
 receivers={}, Receive=function(n,f) net.receivers[n]=f end}
local rq,ri
_G.FEED=function(q) rq,ri=q,0 end
local function rd() ri=ri+1 return rq[ri] end
net.ReadString=rd net.ReadVector=rd net.ReadUInt=rd net.ReadBool=rd net.ReadInt=rd
_G.FILES={}
-- fix64: file.AsyncRead (callbacks delivered by RUNASYNC), file.Read / Write in DATA
_G.FSASYNC_OK=0
_G.FSASYNC_ERR_FILEOPEN=-1
_G.ASYNCQ={}
_G.ASYNCREADS=0
_G.DATA={}
_G.RUNASYNC=function(n) local q=ASYNCQ _G.ASYNCQ={} for i,a in ipairs(q) do if n and i>n then ASYNCQ[#ASYNCQ+1]=a else local d=FILES[a.p] a.cb(a.p,a.g,d and FSASYNC_OK or FSASYNC_ERR_FILEOPEN,d) end end end
_G.file={AsyncRead=function(p,g,cb) ASYNCREADS=ASYNCREADS+1 ASYNCQ[#ASYNCQ+1]={p=p,g=g,cb=cb} return FSASYNC_OK end, Read=function(p,g) return DATA[p] end, Write=function(p,t) DATA[p]=t end,
 Open=function(p,m,w) local d=FILES[p] if not d then return nil end return {Size=function() return #d end, Read=function(_,n) return d:sub(1,n) end, Close=function() end} end}
_G.ENTS={}
local EM={} EM.__index=EM
function EM:IsValid() return not self.removed end
function EM:IsWorld() return self.idx==0 end
function EM:EntIndex() return self.idx end
function EM:WorldSpaceCenter() return self.pos end
function EM:GetPos() return self.pos end
function EM:EyePos() return self.pos+Vector(0,0,64) end
function EM:IsDormant() return self.dormant or false end
function EM:GetOwner() return self.owner end
function EM:Alive() return true end
function EM:StopSound(n) L('origStop',n) end
function EM:GetNW2Int(k,d) return (self.nw or {})[k] or d end
function EM:GetNW2Entity(k,d) return (self.nw or {})[k] or d end
function EM:GetActiveWeapon() return self.active end
function _G.MKENT(idx,pos) local e=setmetatable({idx=idx,pos=pos},EM) ENTS[idx]=e return e end
function _G.IsValid(e) return e~=nil and e~=false and (type(e)~='table' or not e.IsValid or e:IsValid()) end
function _G.Entity(i) return ENTS[i] end
_G.NULL=setmetatable({idx=-1,removed=true},EM)
function _G.FindMetaTable(n) if n=='Entity' then return EM end if n=='CSoundPatch' then return _G.PATCHM end end
_G.PATCHM={Stop=function(self) L('patchStop') end, FadeOut=function(self,s) L('patchFade',s) end}
_G.PATCHM.__index=_G.PATCHM
function _G.CreateSound(ent,name) return setmetatable({},PATCHM) end
_G.sound={Play=function(n) L('origPlay',n) end, GetProperties=function(n) if n=='Script.X' then return {sound={'^weapons/a.wav','weapons/b.wav'}} end end}
local timers={}
_G.timer={Simple=function(t,f) timers[#timers+1]={T+t,f} end}
_G.RUNTIMERS=function() for i=#timers,1,-1 do if timers[i][1]<=T then local f=timers[i][2] table.remove(timers,i) f() end end end
function _G.LocalPlayer() return LP end
function _G.engine_tick() end
_G.engine={TickCount=function() return frame end}
_G.ents={FindByClass=function(c) local r={} for _,e in pairs(ENTS) do if e.class==c then r[#r+1]=e end end return r end}
_G.STTS={}
_G.player={}
-- strict globals
setmetatable(_G,{__index=function(_,k) error('undefined global read: '..tostring(k),2) end})
