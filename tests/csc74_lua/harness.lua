-- mocked GMod: one server + two clients (A = in the call, B = standing next to A) + a fake native module
local ROOT = ...
local LOG = {}
local function log(...) local t = {} for i = 1, select('#', ...) do t[#t + 1] = tostring(select(i, ...)) end LOG[#LOG + 1] = table.concat(t, " ") end
local function fail(msg) error("TEST FAIL: " .. msg, 2) end
local function expect(c, msg) if not c then fail(msg) end end

-- ---------- math types
local V = {} V.__index = V
function Vector(x, y, z) return setmetatable({x = x or 0, y = y or 0, z = z or 0}, V) end
V.__add = function(a, b) return Vector(a.x + b.x, a.y + b.y, a.z + b.z) end
V.__sub = function(a, b) return Vector(a.x - b.x, a.y - b.y, a.z - b.z) end
V.__mul = function(a, b) if type(a) == "number" then a, b = b, a end return Vector(a.x * b, a.y * b, a.z * b) end
V.__tostring = function(a) return string.format("(%g %g %g)", a.x, a.y, a.z) end
function V:Length() return math.sqrt(self.x ^ 2 + self.y ^ 2 + self.z ^ 2) end
function V:LengthSqr() return self.x ^ 2 + self.y ^ 2 + self.z ^ 2 end
function V:Distance(b) return (self - b):Length() end
function V:DistToSqr(b) return (self - b):LengthSqr() end
function V:GetNormalized() local l = self:Length() if l == 0 then return Vector() end return Vector(self.x / l, self.y / l, self.z / l) end
function V:Dot(b) return self.x * b.x + self.y * b.y + self.z * b.z end
local A = {} A.__index = A
function Angle(p, y, r) return setmetatable({p = p or 0, y = y or 0, r = r or 0}, A) end
function A:Forward() local p, y = math.rad(self.p), math.rad(self.y) return Vector(math.cos(p) * math.cos(y), math.cos(p) * math.sin(y), -math.sin(p)) end
function LerpVector(k, a, b) return a + (b - a) * k end
function LerpAngle(k, a, b) return Angle(a.p + (b.p - a.p) * k, a.y + (b.y - a.y) * k, a.r + (b.r - a.r) * k) end
function Lerp(k, a, b) return a + (b - a) * k end
local M = {} M.__index = M
function Matrix() return setmetatable({t = Vector(), a = Angle()}, M) end
function M:SetTranslation(v) self.t = v end
function M:SetAngles(a) self.a = a end
function M:GetTranslation() return self.t end
function M:GetAngles() return self.a end
function M:GetInverseTR() local m = Matrix() m.t = self.t * -1 return m end
M.__mul = function(a, b) local m = Matrix() m.t = a.t + b.t return m end
function Color(r, g, b, a) return {r = r, g = g, b = b, a = a or 255} end
function math.Clamp(v, lo, hi) return math.min(math.max(v, lo), hi) end
function math.Round(v) return math.floor(v + 0.5) end
function string.Explode(sep, s)
	local out, i = {}, 1
	while true do
		local j = string.find(s, sep, i, true)
		if not j then out[#out + 1] = string.sub(s, i) break end
		out[#out + 1] = string.sub(s, i, j - 1)
		i = j + #sep
	end
	return out
end
function table.Count(t) local n = 0 for _ in pairs(t) do n = n + 1 end return n end
FCVAR_ARCHIVE, RENDERGROUP_OPAQUE, TEXT_ALIGN_LEFT, TEXT_ALIGN_RIGHT = 128, 1, 0, 2
function ErrorNoHalt(s) log("ERRORNOHALT", s) error("ErrorNoHalt: " .. tostring(s)) end
function IsValid(e) return e ~= nil and e ~= false and (type(e) ~= "table" or e.valid ~= false) and (type(e) ~= "table" or not e.IsValid or e:IsValid()) end

local now = 100
function RealTime() return now end
function CurTime() return now end
local frame = 1
function FrameNumber() return frame end

-- ---------- entities
local Ent = {} Ent.__index = Ent
local entList, nextIdx = {}, 1
local function NewEnt(t)
	t = t or {}
	t.idx = t.idx or nextIdx nextIdx = math.max(nextIdx, t.idx) + 1
	t.nw = t.nw or {}
	t.pos = t.pos or Vector()
	t.ang = t.ang or Angle()
	setmetatable(t, Ent)
	entList[#entList + 1] = t
	return t
end
function Ent:IsValid() return self.valid ~= false end
function Ent:EntIndex() return self.idx end
function Ent:GetClass() return self.class or "prop_physics" end
function Ent:IsPlayer() return self.isPlayer == true end
function Ent:IsBot() return false end
function Ent:IsNPC() return false end
function Ent:IsWeapon() return self.isWeapon == true end
function Ent:IsRagdoll() return false end
function Ent:IsWorld() return false end
function Ent:IsDormant() return false end
function Ent:MapCreationID() return -1 end
function Ent:CreatedByMap() return false end
function Ent:Alive() return self.alive ~= false end
function Ent:Nick() return self.name or "P" end
function Ent:SteamID64() return self.sid or "76561198000000001" end
function Ent:GetPos() return self.pos end
function Ent:GetAngles() return self.ang end
function Ent:EyePos() return self.pos + Vector(0, 0, 64) end
function Ent:WorldSpaceCenter() return self.pos + Vector(0, 0, 36) end
function Ent:GetModel() return self.model or "models/props_c17/oildrum001.mdl" end
function Ent:GetSkin() return 0 end
function Ent:GetSequence() return 3 end
function Ent:GetCycle() return 0.5 end
function Ent:SetupBones() end
function Ent:LookupBone(n) if self.isPlayer or self.ghost then return (self.bones or {})[n] or (n:find("ValveBiped") and (#n % 20)) end end
function Ent:GetBoneMatrix(i) local m = Matrix() m.t = self.pos + Vector(0, 0, i) return m end
function Ent:GetBoneParent(i) return i > 0 and i - 1 or -1 end
function Ent:SetBoneMatrix(i, m) self.setBones = (self.setBones or 0) + 1 end
function Ent:GetActiveWeapon() return self.active end
function Ent:GetWeapon(cls) return self.phone end
function Ent:HasWeapon() return self.phone ~= nil end
function Ent:GetOwner() return self.owner end
function Ent:GetParent() return self.parent end
function Ent:EmitSound(s) log("EMIT", self.name, s) end
function Ent:GetNoDraw() return self.nodraw == true end
function Ent:SetNoDraw(b) self.nodraw = b end
function Ent:SetPos(p) self.pos = p end
function Ent:SetAngles(a) self.ang = a end
function Ent:SetSkin() end
function Ent:SetSequence() end
function Ent:SetCycle() end
function Ent:InvalidateBoneCache() end
function Ent:Remove() self.valid = false end
function Ent:AddCallback(name, fn) self.cb = fn end
function Ent:UserID() return self.idx end
for _, kind in ipairs({"Bool", "Int", "Float", "String", "Entity"}) do
	Ent["GetNW2" .. kind] = function(self, k, d) local v = self.nw[k] if v == nil then if d == nil then if kind == "Bool" then return false elseif kind == "String" then return "" elseif kind == "Entity" then return nil end return 0 end return d end return v end
	Ent["SetNW2" .. kind] = function(self, k, v) self.nw[k] = v end
end
Ent.GetNWString = Ent.GetNW2String

-- ---------- realms
local function shared(realm)
	local env = setmetatable({}, {__index = _G})
	env.SERVER, env.CLIENT = realm == "server", realm ~= "server"
	env.STTS = {}
	local hooks, timers, cmds = {}, {}, {}
	env.hook = {Add = function(ev, name, fn) hooks[ev] = hooks[ev] or {} hooks[ev][name] = fn end, Remove = function() end}
	env.timer = {Create = function(name, d, r, fn) timers[name] = {d = d, fn = fn, at = now + d} end, Simple = function(d, fn) timers[{}] = {d = d, fn = fn, at = now + d, once = true} end, Remove = function(n) timers[n] = nil end}
	env.concommand = {Add = function(n, fn) cmds[n] = fn end}
	env.cvars = {AddChangeCallback = function() end}
	env.print = function(...) log("PRINT[" .. realm .. "]", ...) end
	env._hooks, env._timers, env._cmds = hooks, timers, cmds
	local cv = {}
	local function mk(name, def)
		local c = {v = tostring(def)}
		function c:GetBool() return tonumber(self.v) ~= 0 end
		function c:GetInt() return math.floor(tonumber(self.v) or 0) end
		function c:GetFloat() return tonumber(self.v) or 0 end
		cv[name] = c
		return c
	end
	env.CreateConVar = function(n, d) return mk(n, d) end
	env.CreateClientConVar = function(n, d) return mk(n, d) end
	env.GetConVar = function(n) return cv[n] end
	env.RunConsoleCommand = function(n, v) if cv[n] then cv[n].v = v end end
	env._cv = cv
	return env
end
local SV = shared("server")
local CA = shared("client")
local CB = shared("client")
local globals2 = {}
function GetGlobal2Bool(k, d) local v = globals2[k] if v == nil then return d end return v end
function SetGlobal2Bool(k, v) globals2[k] = v end
function GetHostName() return "Test Server" end
game = {GetMap = function() return "gm_construct" end}
util = {AddNetworkString = function() end, NetworkStringToID = function() return 5 end, IsValidModel = function(m) return m ~= "models/missing.mdl" end}
function ClientsideModel(mdl) local e = NewEnt({model = mdl, ghost = true, class = "class C_BaseFlex"}) e.ghostOf = mdl return e end
local B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
function util.Base64Encode(d, noNl)
	local out = {}
	for i = 1, #d, 3 do
		local a, b, c = d:byte(i, i + 2)
		local n = a * 65536 + (b or 0) * 256 + (c or 0)
		local q = {math.floor(n / 262144) % 64, math.floor(n / 4096) % 64, math.floor(n / 64) % 64, n % 64}
		out[#out + 1] = B64:sub(q[1] + 1, q[1] + 1) .. B64:sub(q[2] + 1, q[2] + 1) .. (b and B64:sub(q[3] + 1, q[3] + 1) or "=") .. (c and B64:sub(q[4] + 1, q[4] + 1) or "=")
	end
	return table.concat(out)
end
function SysTime() return now end
-- fix75: a fake JPEG (SOI + filler)
local function fakeJpeg(n) return "\255\216\255\224" .. string.rep("j", n - 4) end
-- the client render side cl_phone_csc74.lua uses (captures, render targets, the DHTML panel)
local function fakeRender(env, tag)
	local R = {captures = 0, pushes = 0, depth = 0, panels = {}, draws = 0}
	env._render = R
	env.render = {
		PushRenderTarget = function(rt) expect(rt and rt.GetName, tag .. ": push a render target") R.pushes = R.pushes + 1 R.depth = R.depth + 1 R.rt = rt end,
		PopRenderTarget = function() R.depth = R.depth - 1 expect(R.depth >= 0, tag .. ": pop without push") end,
		Clear = function() end,
		Capture = function(t)
			expect(R.depth > 0, tag .. ": capture outside a render target")
			expect(t.format == "jpg" and t.w > 0 and t.h > 0 and t.quality >= 10 and t.quality <= 90, tag .. ": capture args")
			R.captures = R.captures + 1 R.lastCap = t
			return fakeJpeg(math.floor(t.w * t.h * t.quality / 400))
		end,
	}
	env.cam = {Start2D = function() R.in2d = true end, End2D = function() R.in2d = false end}
	env.surface = {SetMaterial = function(m) expect(m ~= nil, tag .. ": material") R.mat = m end, SetDrawColor = function() end,
		DrawTexturedRectUV = function(x, y, w, h, u0, v0, u1, v1) expect(R.in2d, tag .. ": draw outside 2D") expect(u1 > 0 and u1 <= 1 and v1 > 0 and v1 <= 1, tag .. ": uv") R.draws = R.draws + 1 end}
	env.RT_SIZE_LITERAL, env.MATERIAL_RT_DEPTH_NONE, env.IMAGE_FORMAT_RGB888 = 5, 2, 2
	env.GetRenderTargetEx = function(name, w, h) return {GetName = function() return name end, Width = function() return w end, Height = function() return h end} end
	env.CreateMaterial = function(name, shader, params) expect(params["$basetexture"], tag .. ": material texture") return {name = name, tex = params["$basetexture"]} end
	env.vgui = {Create = function(cls)
		expect(cls == "DHTML", tag .. ": panel class")
		local p = {valid = true, js = {}, updates = 0}
		function p:IsValid() return self.valid end
		for _, f in ipairs({"SetSize", "SetPos", "SetPaintedManually", "SetMouseInputEnabled", "SetKeyboardInputEnabled", "SetAllowLua"}) do p[f] = function() end end
		function p:SetHTML(h) self.html = h self.readyAt = now + 0.1 end
		function p:QueueJavascript(js) expect(self.valid, tag .. ": js on a removed panel") self.js[#self.js + 1] = js end
		function p:UpdateHTMLTexture() self.updates = self.updates + 1 end
		function p:GetHTMLMaterial() return {name = "__html", GetTexture = function() return {Width = function() return 512 end, Height = function() return 512 end} end} end
		function p:Remove() self.valid = false end
		R.panels[#R.panels + 1] = p
		return p
	end}
end

-- players + phones
local plyA = NewEnt({isPlayer = true, name = "Alice", pos = Vector(0, 0, 0), sid = "76561198000000011"})
local plyB = NewEnt({isPlayer = true, name = "Bob", pos = Vector(100, 0, 0), sid = "76561198000000022"})
local wepA = NewEnt({class = "weapon_stts_phone", isWeapon = true, owner = plyA})
plyA.phone, plyA.active = wepA, wepA
local prop = NewEnt({class = "prop_physics", pos = Vector(200, 0, 0)})
local players = {plyA, plyB}
player = {Iterator = function() local i = 0 return function() i = i + 1 local p = players[i] if p then return i, p end end end, GetHumans = function() return players end}
ents = {FindInSphere = function(p, r) local out = {} for _, e in ipairs(entList) do if e.valid ~= false and e.pos:Distance(p) <= r then out[#out + 1] = e end end return out end,
	FindByClass = function() return {wepA} end}

-- ---------- net
local queue = {}
local function newNet(realm, env, owner)
	local n = {recv = {}}
	local out
	function n.Start(name, unrel) out = {name = name, vals = {}, unrel = unrel} end
	for _, k in ipairs({"UInt", "Int", "String", "Bool", "Float", "Vector", "Angle"}) do
		n["Write" .. k] = function(v) out.vals[#out.vals + 1] = v end
	end
	function n.WriteData(d, len) expect(#d == len, "WriteData length " .. #d .. " ~= " .. tostring(len)) out.vals[#out.vals + 1] = d end
	local cur
	local function pop() cur.i = cur.i + 1 local v = cur.vals[cur.i] if v == nil then fail(realm .. ": read past the end of " .. cur.name) end return v end
	for _, k in ipairs({"UInt", "Int", "String", "Bool", "Float", "Vector", "Angle"}) do n["Read" .. k] = pop end
	function n.ReadData(len) local d = pop() expect(#d == len, "ReadData length") return d end
	function n.Receive(name, fn) n.recv[name] = fn end
	function n.SendToServer() queue[#queue + 1] = {to = "server", from = owner, msg = out} end
	function n.Send(to) if type(to) == "table" and not to.isPlayer then for _, p in ipairs(to) do queue[#queue + 1] = {to = p, msg = out} end else queue[#queue + 1] = {to = to, msg = out} end end
	function n._deliver(msg, from) cur = {vals = msg.vals, i = 0, name = msg.name} local fn = n.recv[msg.name] if fn then fn(0, from) end expect(cur.i == #cur.vals, realm .. ": unread values in " .. msg.name .. " (" .. cur.i .. "/" .. #cur.vals .. ")") end
	env.net = n
	return n
end
local nSV = newNet("server", SV)
local nCA = newNet("clientA", CA, plyA)
local nCB = newNet("clientB", CB, plyB)
CA.LocalPlayer = function() return plyA end
CB.LocalPlayer = function() return plyB end
CA.EyePos = function() return plyA:EyePos() end
CB.EyePos = function() return plyB:EyePos() end
local function pump()
	for _ = 1, 50 do
		if #queue == 0 then return end
		local q = queue queue = {}
		for _, item in ipairs(q) do
			if item.to == "server" then nSV._deliver(item.msg, item.from)
			elseif item.to == plyA then nCA._deliver(item.msg)
			elseif item.to == plyB then nCB._deliver(item.msg) end
		end
	end
end

-- ---------- shared geo
local G = {CALL_NONE = 0, CALL_OUT = 1, CALL_IN = 2, CALL_ON = 3, APP_HOME = 0, APP_CONTACTS = 1, APP_CSC = 25, APP_EXPERIMENTAL = 26, ZoomMax = 10,
	CamWorld = function(p, a) return p, a end}
for _, env in ipairs({SV, CA, CB}) do env.STTS.PhoneGeo = G end

-- ---------- server side of sv_phone.lua that CSC uses
local PH = {}
SV.STTS.Phone = PH
local sent = {}
PH.Send74 = function(ply, role, state, since, name) sent[#sent + 1] = {ply = ply, role = role, state = state, name = name} end
PH.Awake74 = function(p) return IsValid(p) and p:Alive() end
PH.PhoneOf74 = function(p) return p.phone end
PH.Holding74 = function(p, w) return p.active ~= nil and p.active == p.phone and (w == nil or w == p.active) end
PH.IsBusy = function(p) return false end
PH.InCall = function(p) return false end
PH.Drop = function(p) end
PH.Ringing = {}
PH.Loops = {}
PH.LoopStart = function(ent, file) local lp = {ent = ent, file = file} PH.Loops[lp] = true log("RING START", file) return lp end
PH.LoopStop = function(lp) PH.Loops[lp] = nil log("RING STOP") end
PH.LoopVol = function() end
PH.PhoneVol = function() return 1 end
PH.PlyVol = function() return 1 end
PH.WorldPos = function(w) return w.owner and w.owner:EyePos() or w.pos end
PH.SyncPhone = function(w) if not (PH.CscSync74 and PH.CscSync74(w)) then w.nw.CallSt = 0 end end
PH.Line73 = {Geo = function(e) return {wep = e.wep, pos = e.wep.owner:EyePos(), holder = e.wep.owner} end,
	Listeners = function(K) local out = {} for _, p in ipairs(players) do local d = p:EyePos():Distance(K.pos) if p == K.holder then out[p] = 1 elseif d < 280 then out[p] = 1 - d / 280 end end return out end}

-- ---------- fake native module (shared by A only; B has it too for line_stream)
local function fakeModule(name)
	local m = {csc_api = 2, build = "fix75-csc-video", line_api = 1, ear_api = 2, calls = {}, events = {}, state = 0, streams = 0, vq = {}, vsent = {}}
	local function ev(e) m.events[#m.events + 1] = e end
	m.ev = ev
	local function rec(n) m.calls[n] = (m.calls[n] or 0) + 1 end
	function m.csc_init(sid) rec("init") expect(type(sid) == "string" and #sid == 17, "sid") return true, "flat" end
	function m.csc_publish(b) rec("publish") if b then ev({kind = "published"}) end end
	function m.csc_info(n, s, mp, busy, ok) rec("info") m.info = {n, s, mp, busy, ok} end
	function m.csc_flags() rec("flags") end
	function m.csc_list() rec("list") ev({kind = "list", list = {{lobby = "109775241000000001", sid = "76561198000000099", name = "Remote Rick", server = "Other Server", map = "gm_construct", busy = false, callable = true}}}) return true end
	function m.csc_call(l) rec("call") expect(l == "109775241000000001", "lobby") m.state = 1 return true end
	function m.csc_answer() rec("answer") if m.state ~= 4 then return false end m.state = 5 ev({kind = "accepted"}) return true end
	function m.csc_decline(r) rec("decline") m.state = 0 ev({kind = "ended", reason = "declined", data = "local"}) return true end
	function m.csc_hangup() rec("hangup") if m.state ~= 0 then m.state = 0 ev({kind = "ended", reason = "local", data = "local"}) end end
	function m.csc_poll() rec("poll") local e = m.events m.events = {} return e end
	function m.csc_status() return {state = m.state, peerMap = "gm_other", peer = "76561198000000099", peerName = "Remote Rick", audioIn = 10, audioOut = 12, videoIn = #m.vsent, videoOut = 3, videoLost = 1, videoFixed = 1, videoBytesIn = 9000, videoBytesOut = 12000} end
	function m.csc_vsend(d) rec("vsend") expect(type(d) == "string" and d:byte(1) == 255 and d:byte(2) == 216 and #d <= 67840, name .. ": vsend jpeg") m.vsent[#m.vsent + 1] = #d return m.state == 5 end
	function m.csc_vtake() rec("vtake") local d = table.remove(m.vq, 1) if d then return d, 1 end return nil end
	function m.csc_audio(mic, game, muf, ear) rec("audio") m.gains = {mic, game, muf, ear} return true end
	function m.csc_line(g) rec("line") m.lineGain = g end
	function m.csc_take() rec("take") return string.rep("\1", 166 * 3) end
	function m.csc_send(k, d, rel) rec("send" .. k) expect(#d <= 1400, "send size " .. #d) m.lastSend = m.lastSend or {} m.lastSend[k] = d if k == 3 and d:find("models/") then m.sceneModels = true end return m.state == 5 end
	function m.line_stream(id, d, g) rec("stream") m.streams = m.streams + 1 expect(#d % 166 == 0, "stream frames") expect(id >= 1, "stream id") end
	function m.line_open() return 1 end
	return m
end
local mA, mB = fakeModule("A"), fakeModule("B")
local earCalls = {}
local function fakeEar(tag)
	return {LineBegin = function() return true end, CscBegin = function(m) earCalls[#earCalls + 1] = tag .. ":begin" return true end,
		CscEnd = function() earCalls[#earCalls + 1] = tag .. ":end" end, CscActive = function() return true end,
		CscStep = function(pos, pocket) earCalls.step = (earCalls.step or 0) + 1 end}
end
CA.STTS.PhoneLine73 = {Module = function() return mA end}
CB.STTS.PhoneLine73 = {Module = function() return mB end}
CA.STTS.PhoneEar63, CB.STTS.PhoneEar63 = fakeEar("A"), fakeEar("B")
local R = {Talking62 = function() return true end, EarMode63 = function() return 1 end, Near63 = function(r, pos) return pos and 1 or 0 end}
CA.STTS.PhoneRecorder31, CB.STTS.PhoneRecorder31 = R, R
-- the phone screen (what cl_phone_csc74.lua uses)
local function fakeScreen(env)
	local SCR = {Remote = {}, APPS = {}, LEVEL = {}, ROOT_ICON = {}, SettingsBack = {}, L = {app = 0, wep = nil}}
	local drawn = {}
	local U = {W = 432, H = 960, C = {label = 1, label2 = 2, label3 = 3, sep = 4, blue = 5}}
	for _, f in ipairs({"R", "T", "Round", "Circle", "Line", "Chevron", "Arc", "LargeTitle", "NavBack"}) do U[f] = function(...) drawn[#drawn + 1] = {f, ...} end end
	U.Fit = function(t) return t end
	U.HoverK = function() return 0 end
	U.Pressed = function() return false end
	U.btns = {}
	U.Reg = function(btns, id, x, y, w, h, fn) btns[id] = fn end
	U.SetApp = function(st, app) if st.own then SCR.L.app = app end end
	U.List = function(btns, wep, st, list, onPick, top, opts) SCR.lastList = list SCR.lastPick = onPick SCR.lastEmpty = opts and opts.empty end
	SCR.UI = U
	SCR.Ink = function(a) return a end
	SCR._drawn = drawn
	SCR.wants = {}
	SCR.WantFeed = function(idx, back, rate) SCR.wants[#SCR.wants + 1] = {idx, back, rate} if idx < 10000 then return {name = "feed_" .. idx} end end
	env.STTS.PhoneScreen = SCR
	return SCR
end
local SA, SB = fakeScreen(CA), fakeScreen(CB)
fakeRender(CA, "A") fakeRender(CB, "B")

local function load(env, path)
	local f = assert(io.open(ROOT .. "/" .. path, "rb")) local src = f:read("*a") f:close()
	local fn, err = loadstring(src, "@" .. path)
	if not fn then fail(err) end
	setfenv(fn, env)
	fn()
end
load(SV, "stts_phone/sv_phone_csc74.lua")
load(CA, "stts_phone/cl_phone_csc74.lua")
load(CB, "stts_phone/cl_phone_csc74.lua")
local C74A, C74B = CA.STTS.PhoneCSC74, CB.STTS.PhoneCSC74

local function think(n)
	for _ = 1, n or 1 do
		now = now + 0.05 frame = frame + 1
		for _, env in ipairs({CA, CB, SV}) do
			for _, fn in pairs(env._hooks.Think or {}) do fn() end
			for k, t in pairs(env._timers) do if now >= t.at then t.at = now + t.d t.fn() if t.once then env._timers[k] = nil end end end
			if env._render then
				for _, p in ipairs(env._render.panels) do if p.valid and p.readyAt and now >= p.readyAt then p.readyAt = nil if p.OnDocumentReady then p:OnDocumentReady("about:blank") end end end
				for _, fn in pairs(env._hooks.PostRender or {}) do fn() end
				expect(env._render.depth == 0, "render target left pushed")
			end
		end
		pump()
	end
end

-- 1. the setting is off: nothing starts
think(5)
expect((mA.calls.init or 0) == 0, "no init while off")
-- 2. A and B switch it on
CA._cv.zhphone_csc.v = "1" CB._cv.zhphone_csc.v = "1"
think(20)
expect(mA.calls.init == 1 and mA.calls.publish >= 1, "A init + publish")
expect(plyA.nw.CSC74 == true and plyB.nw.CSC74 == true, "server knows the setting")
expect(plyB.STTS_CscHear74 == true, "B can hear CSC")
expect(plyA.nw.CscOk74 == true, "A callable")
expect(C74A.S.published == true, "published event")
-- 3. A opens Contacts > CSC: the row + the list
local btns, stA = {}, {own = true, sel = 0}
local t = SA.CscRow74(btns, wepA, stA, 180)
expect(t > 180 and btns[96], "row registered")
SA.L.wep = wepA
btns[96]()
expect(SA.L.app == 25, "row opens APP_CSC")
think(4)
expect(mA.calls.list >= 1 and #C74A.S.list == 1, "list arrived")
SA.APPS[25](wepA, stA, {})
expect(#SA.lastList == 1 and SA.lastList[1].name == "Remote Rick", "list drawn")
expect(SB.S == nil, "sanity")
-- B (bystander) sees A's phone in APP_CSC: the relayed list
SB.APPS[25](wepA, {own = false, sel = 0}, {})
expect(#SB.lastList == 1 and SB.lastList[1].sub:find("Other Server"), "bystander list relayed")
-- 4. A taps Rick
SA.lastPick(SA.lastList[1])
think(2)
expect(wepA.nw.CallSt == 1, "server: calling out (CallSt " .. tostring(wepA.nw.CallSt) .. ")")
expect(wepA.nw.CallName:find("Remote Rick"), "call name")
mA.state = 3 mA.ev({kind = "ringing"})
think(2)
expect(sent[#sent].state == "ringing" and sent[#sent].role == "out", "ringback")
mA.state = 5 mA.ev({kind = "accepted"})
think(6)
expect(wepA.nw.CallSt == 3, "connected")
expect(wepA.nw.PeerIdx == 10000 + wepA.idx, "peer idx")
expect(plyA.STTS_CscWep74 == wepA, "ear phone")
expect(earCalls[1] == "A:begin", "ear begin")
expect(mA.calls.audio and mA.gains[1] > 0, "mic gain")
expect(wepA.nw.CscRelay74 == true, "relay on (B is near)")
expect(mB.streams > 0, "B hears the other side (" .. mB.streams .. ")")
expect(mA.lineGain and mA.lineGain > 0.9, "own line gain " .. tostring(mA.lineGain))
-- 5. video (fix75): the real picture
local feed = 10000 + wepA.idx
mA.ev({kind = "msg", msgKind = 1, data = "1 1 0"}) -- a fix74 peer: its "video" was a scene description
think(3)
expect(not wepA.nw.PeerVid, "fix74 peer: no picture")
mA.ev({kind = "msg", msgKind = 1, data = "1 1 0 2"})
think(3)
expect(wepA.nw.PeerVid == true, "peer video on (fix75 peer, another map)")
expect(SA.CscFeed74(feed) == nil, "nothing to show before the first picture")
for i = 1, 30 do mA.vq[#mA.vq + 1] = fakeJpeg(3000 + i) think(1) end
local matA = SA.CscFeed74(feed)
expect(matA and matA.name == "stts_csc75_feedm_1", "A shows the other side's picture")
local pA = CA._render.panels[1]
expect(pA and #CA._render.panels == 1, "one panel (" .. #CA._render.panels .. ")")
local pushed = 0 for _, js in ipairs(pA.js) do if js:find("setFrame('/9j/", 1, true) then pushed = pushed + 1 end end
expect(pushed >= 20 and pA.updates > 0, "pictures pushed to the panel (" .. pushed .. ")")
expect(C74A.S.stats.videoIn >= 30, "A video in " .. C74A.S.stats.videoIn)
-- B stands next to A: the smaller copy through the server
expect(wepA.nw.CscView74 == true, "B looks at A's phone")
expect((C74A.S.stats.videoRelay or 0) > 0, "A relays a copy")
local matB = SB.CscFeed74(feed)
expect(matB and matB.name == "stts_csc75_feedm_1", "B shows the relayed picture")
expect(C74B.S.stats.videoIn >= 3, "B video in " .. C74B.S.stats.videoIn)
expect(C74B.S.stats.videoIn <= 30 * 0.05 * 4 + 3, "relay is ~4 a second (" .. C74B.S.stats.videoIn .. ")")
expect((C74B.S.stats.videoBytesIn or 0) / C74B.S.stats.videoIn <= 16000, "relay size")
-- the server drops what is not a JPEG
local before = C74B.S.stats.videoIn
CA.net.Start("stts_csc74") CA.net.WriteUInt(7, 4) CA.net.WriteUInt(100, 16) CA.net.WriteData(string.rep("x", 100), 100) CA.net.SendToServer()
pump()
expect(C74B.S.stats.videoIn == before, "server dropped a non-JPEG")
-- the same picture is not sent to the holder (A gets it straight from the other side)
local ownA = C74A.S.stats.videoIn
think(2)
expect(C74A.S.stats.videoIn == ownA, "no relay back to A")
-- own camera: VidOn -> pictures to the other side
wepA.nw.VidOn = true
think(10)
expect(mA.lastSend[1] == "1 1 0 2", "camera state " .. tostring(mA.lastSend[1]))
expect(C74A.S.sendVid and C74A.S.sendVid.idx == wepA.idx, "sending")
expect((mA.calls.vsend or 0) >= 4, "own pictures sent (" .. tostring(mA.calls.vsend) .. ")")
expect(CA._render.lastCap.w == 216 and CA._render.lastCap.h == 384, "preset 2 size")
local wantOwn = false for _, w in ipairs(SA.wants) do if w[1] == wepA.idx and w[2] == true and w[3] == 12 then wantOwn = true end end
expect(wantOwn, "own feed kept rendering at 12 fps")
CA._cv.zhphone_csc_video_q.v = "1"
think(6)
expect(CA._render.lastCap.w == 144 and CA._render.lastCap.h == 256, "preset 1 size")
CA._cv.zhphone_csc_video.v = "0"
think(2)
local nv = mA.calls.vsend
think(6)
expect(mA.calls.vsend == nv and C74A.S.sendVid == nil and mA.lastSend[1] == "0 1 0 2", "zhphone_csc_video 0 stops it")
CA._cv.zhphone_csc_video.v = "1"
plyA.active = nil -- in a pocket
think(4)
nv = mA.calls.vsend
think(6)
expect(mA.calls.vsend == nv and mA.lastSend[1] == "1 1 1 2", "pocket: nothing sent")
plyA.active = wepA
think(4)
expect(mA.calls.vsend > nv, "back in hand: sending again")
CA._cmds.zhphone_csc_stats()
-- 6. hang up from the phone screen
SV.STTS.Phone.CscAct74(plyA, wepA, true, "hangup", "")
think(4)
expect(mA.calls.hangup >= 1, "module hung up")
expect(wepA.nw.CallSt == 0, "call screen closed")
expect(earCalls[#earCalls] == "A:end", "ear end")
expect(SV.STTS.Phone.Csc74.calls[plyA] == nil, "server state cleared")
expect(SA.CscFeed74(feed) == nil and C74A.S.sendVid == nil and C74A.S.peerV2 == nil, "A's picture dropped")
think(440) -- 22 s: nobody looks at B's copy any more
local leftB = 0 for _, sl in ipairs(C74B.slots) do if sl.feed then leftB = leftB + 1 end end
expect(leftB == 0 and CB._render.panels[1].valid == false, "B's idle picture freed")
-- 7. incoming call, answered
mA.state = 4 mA.ev({kind = "ring", name = "Remote Rick", server = "Other Server", sid = "76561198000000099", map = "gm_construct"})
think(2)
expect(wepA.nw.CallSt == 2, "ringing here")
local ringing = false for lp in pairs(PH.Loops) do ringing = true end
expect(ringing, "ringtone")
SV.STTS.Phone.CscAct74(plyA, wepA, true, "answer", "")
think(3)
expect(mA.calls.answer == 1 and wepA.nw.CallSt == 3, "answered")
ringing = false for lp in pairs(PH.Loops) do ringing = true end
expect(not ringing, "ringtone stopped")
-- the other side hangs up
mA.state = 0 mA.ev({kind = "ended", reason = "hangup"})
think(3)
expect(wepA.nw.CallSt == 0, "ended by the other side")
-- 8. busy here: a call comes in during a local call -> declined busy
PH.IsBusy = function(p) return p == plyA end
SV.STTS.Phone.Csc74.orig.IsBusy = PH.IsBusy
mA.state = 4 mA.ev({kind = "ring", name = "X", server = "Y", sid = "1", map = "m"})
think(3)
expect(mA.calls.decline == 1, "declined busy")
expect(wepA.nw.CallSt == 0, "no ring while busy")
-- 9. stats command
CA._cmds.zhphone_csc_stats()
SV._cmds.stts_phone_csc_list()
print(string.format("video: A in %d relay %d vsend %d, B in %d (%d bytes), A panel js %d", C74A.S.stats.videoIn, C74A.S.stats.videoRelay or 0, mA.calls.vsend or 0, C74B.S.stats.videoIn, C74B.S.stats.videoBytesIn or 0, #CA._render.panels[1].js))
for _, l in ipairs(LOG) do if l:find("video") then print(l) end end
print("ALL OK")
for _, l in ipairs(LOG) do if l:find("ERROR") then print(l) end end
