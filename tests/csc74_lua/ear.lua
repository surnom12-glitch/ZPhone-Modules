local ROOT = ...
local function expect(c, m) if not c then error("TEST FAIL: " .. m, 2) end end
local V = {} V.__index = V
function Vector(x, y, z) return setmetatable({x = x or 0, y = y or 0, z = z or 0}, V) end
V.__sub = function(a, b) return Vector(a.x - b.x, a.y - b.y, a.z - b.z) end
V.__add = function(a, b) return Vector(a.x + b.x, a.y + b.y, a.z + b.z) end
function V:Distance(b) local d = self - b return math.sqrt(d.x ^ 2 + d.y ^ 2 + d.z ^ 2) end
function math.Clamp(v, a, b) return math.min(math.max(v, a), b) end
bit = {band = function(a, b) local r, m = 0, 1 while a > 0 and b > 0 do if a % 2 == 1 and b % 2 == 1 then r = r + m end a, b, m = math.floor(a / 2), math.floor(b / 2), m * 2 end return r end}
local now = 10
function RealTime() return now end
function CurTime() return now end
function FrameNumber() return math.floor(now * 100) end
function IsValid(e) return e ~= nil and e.valid ~= false end
MASK_SOLID_BRUSHONLY, FSASYNC_OK = 1, 0
local sentNet = {}
util = {TraceLine = function(t) t.output.Hit = false end, NetworkStringToID = function() return 3 end}
net = {Start = function(n) sentNet[#sentNet + 1] = {n = n} end, WriteBool = function(b) sentNet[#sentNet].v = b end, SendToServer = function() end, Receive = function() end,
	ReadString = function() end}
HOOKS = {}
hook = {Add = function(ev, id, fn) HOOKS[ev] = HOOKS[ev] or {} HOOKS[ev][id] = fn end}
local function run(ev, ...) for _, fn in pairs(HOOKS[ev] or {}) do fn(...) end end
local reads = {}
file = {AsyncRead = function(path, g, cb) reads[#reads + 1] = {path, cb} return 0 end, Open = function() end, Read = function() end, Write = function() end}
timer = {Simple = function() end}
sound = {Play = function() end, GetProperties = function() end}
function FindMetaTable() return {StopSound = function() end, Stop = function() end, FadeOut = function() end} end
function CreateSound() return {} end
function LocalPlayer() return {valid = true, EyePos = function() return Vector(0, 0, 64) end} end
STTS = {}
local f = assert(io.open(ROOT .. "/autorun/client/cl_stts_phone_ear63.lua")) local src = f:read("*a") f:close()
assert(loadstring(src, "@ear63"))()
local E = STTS.PhoneEar63
local calls = {}
local m = {ear_api = 2, ear_load = function(k, d) calls.load = (calls.load or 0) + 1 return 2 end, ear_state = function() return 2, 1.5, false end,
	csc_ear_play = function(id, key, g, lp, p, loop, age) calls.play = (calls.play or 0) + 1 return 1 end,
	csc_ear_set = function() calls.set = (calls.set or 0) + 1 end, csc_ear_stop = function(id) calls.stop = (calls.stop or 0) + 1 end}
expect(E.CscBegin(m) == true, "begin")
expect(sentNet[#sentNet].v == true, "told the server: on")
expect(E.CscActive(), "active")
E.Sound("weapons/pistol/pistol_fire2.wav", Vector(100, 0, 0), nil, 90, 1, 100, 1, 0, false)
run("Think")
expect(#reads == 1, "file read queued")
reads[1][2](nil, nil, 0, string.rep("x", 100)) -- the read finishes
run("Think")
expect((calls.load or 0) >= 1, "file sent to the DLL")
expect((calls.play or 0) >= 1, "csc voice started")
now = now + 0.05
E.CscStep(Vector(0, 0, 60), false) -- pump happens in Think; call the pump hook path:
-- the Think hook was registered through hook.Add (mock: ignored), so settle via Warm path: call E.Sound again (already known file)
E.Sound("weapons/pistol/pistol_fire2.wav", Vector(120, 0, 0), nil, 90, 1, 100, 1, 0, false)
run("Think")
E.CscEnd()
expect((calls.stop or 0) >= 1, "stopped")
expect(sentNet[#sentNet].v == false, "told the server: off")
expect(not E.CscActive(), "inactive")
print("EAR OK", calls.load, calls.play, calls.stop)
