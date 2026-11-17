fix74 / fix75 Cross Server Calls - Lua simulation (LuaJIT through Python "lupa": pip install lupa)

  python3 run.py    <addon>/lua harness.lua   -> ALL OK
      a fake GMod server + two clients + a fake native module (csc_api 2): the setting, the list
      (and the list a bystander sees), call out -> ringing -> accepted, voice relayed to a
      bystander, fix75 video: a fix74 peer = no picture, the other side's JPEG pictures shown
      on the call screen (DHTML panel -> render target), the smaller copy relayed through the
      server to a bystander (~4 a second, <= 16 KB), a non-JPEG dropped by the server, your own
      camera captured and sent (presets 1 / 2, zhphone_csc_video 0, pocket), stats, hang up
      (the picture dropped, an idle copy freed after 20 s), incoming call + answer, remote
      hang up, busy
  python3 runear.py <addon>/lua ear.lua       -> EAR OK
      cl_stts_phone_ear63.lua: the CSC instance (async file read, play, stop, tell the server)

<addon> = the folder that contains lua/ of the fix75 addon.
