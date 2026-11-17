fix63 Lua simulation of the phone ear (no GMod needed): harness.lua = small mocks of the GMod API
(Vector, util.TraceLine, net, file, hook, entities, sound, CreateSound, timer) with strict globals.
  pip install lupa
  python3 run.py test_client.lua <addon>/lua/autorun/client/cl_stts_phone_ear63.lua
  python3 run.py test_server.lua "" <addon>/lua/stts_phone/sv_phone_ear.lua
Client (32 checks): Source fall-off, key normalisation, same-frame and client/server dedup, wall ->
quieter + low-pass, quiet far sounds culled, 2D sounds ignored, bad files refused, a loop following a
moving entity, one-shots ended by their length, StopSound / sound-script stop / CSoundPatch FadeOut,
channel replacement, dormant entities at the server's position, 64-voice cap, no net without fix63
on the server. Server (13 checks): nothing sent while nobody films, loud-at-the-phone filter, tick
dedup, entity follow, channel 136, StopSound / CSoundPatch:Stop relayed, 40 msg/s limit, phone held
by another player, opt-out. Result of fix63: PASS.
fix64 (async path: DLL ear_api 2 + file.AsyncRead; harness.lua got file.AsyncRead / Read / Write mocks):
  python3 run.py test_client64.lua  <addon>/lua/autorun/client/cl_stts_phone_ear63.lua
  python3 run.py test_client64b.lua <addon>/lua/autorun/client/cl_stts_phone_ear63.lua
Client64 (20 checks): no file.Open on the game thread, a voice waits for its file and starts with its
age, a known file plays at once, unusable / missing files never play and are not read again, DLL busy
(4) -> sent the next frame, StopSound and channel replacement while waiting, 6 reads started per frame /
8 in flight, used files remembered. Client64b (5 checks): no read-ahead for a fix63 DLL, remembered
keys read ahead (bad key skipped), a read-ahead file plays in the first frame without a new read.
Result of fix64: PASS (test_client.lua 32 + test_server.lua 13 still pass: the fix63 path is unchanged).
fix73 (the phone line):
  python3 run.py test_line73.lua <addon>/lua/autorun/client/cl_stts_phone_ear63.lua   (loads cl_stts_phone_line73.lua next to it)
  python3 run.py test_line73_server.lua "" <addon>/lua/stts_phone/sv_phone_ear.lua    (runs the PHONE.Line73 block of sv_phone.lua next to it)
Line client (27 checks): capability announce (once, again when it changes, never with a fix70 DLL or
zhphone_line_ear 0), the native line opens once per call, sounds shaped at the OTHER phone (loud near
it, culled far from it), unknown line ids ignored, pocket = muffled + quieter, speaker gain, the clip
ear and the line separate while filming during a call, loops / stops / channel replacement on an
entity this client never saw (server index), master = zhphone_line_gain x volume (0 without focus),
band, a lost native line reopened after 1 s, closed at the end of the call / on a stale state.
Line server (42 checks): holders hear each other, bystanders by the mic curve and the speaker
distance, phone on the floor, phone in a pocket, walls, phone volume (0 -> holder still 0.3), gag,
brain damage, unconscious, bots, VoiceRoute72 (2D on the line, 3D within 360 units, nil otherwise),
ears only for fix73 clients and not within 600 units of the other phone, the 10 Hz timer (voice
levels + line states), the relay (near / far / the listener's own sounds not echoed / StopSound with the entity index / 40 msg/s), nothing
after the call, 911 caller <-> operator headset. Result of fix73: PASS; the fix63/fix64 tests above
still pass unchanged (32 + 20 + 5 + 13).
