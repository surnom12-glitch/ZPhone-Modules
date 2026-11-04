# ZPhone for Z-City (fix63)

A smartphone for the Garry's Mod gamemode **Z-City**: calls, 911, TeleChat, camera, photo/video,
gallery, settings, control centre, clock, music. Standalone addon: Z-City files are not modified.

**fix62:** phone videos record **real sound** — your microphone (while you talk in voice chat) and
the game's own audio — as AAC inside the MP4, in sync with the video, through the optional native
recorder (H.264 NVENC).

**fix63 — the phone's own ear:** the game sound of a clip is heard **where the phone is**, not at
your head (like the camera in *Content Warning*). Every sound the game starts is re-played by the
native module into the clip at the phone's position: Source's distance fall-off, walls muffle,
far sounds lose their highs, moving sources are followed, pitch and loops as in the game. Sounds
only the server plays (NPCs, doors, other players' steps and shots) are relayed by the server
(fix63 addon needed there). While the phone is at your head (< ~70 units) the clip uses "what you
hear" (with the other players' voice chat), beyond ~170 units only the phone's ear, crossfaded in
between. Not heard by the ear (only near you): other players' voice chat, BASS streams
(`sound.PlayFile` / `PlayURL`: radio, music, Glide engines), map ambients, sounds started before
recording; room DSP/echo is not re-created.

## Install

1. Close Garry's Mod.
2. Run **`install.bat`**. It finds Garry's Mod through Steam (all library folders), moves the old
   `zcity_phone` addon and old DLLs to `GarrysMod\zphone_backup_<date>\` and copies:

   |            From this package               |                   To                      |
   |--------------------------------------------|-------------------------------------------|
   | `garrysmod\addons\zcity_phone\`            | `GarrysMod\garrysmod\addons\zcity_phone\` |
   | `garrysmod\lua\bin\gmcl_zhphone_win64.dll` | `GarrysMod\garrysmod\lua\bin\`            |
   | `bin\win64\av*-zhphone32-*.dll` (3 files)  | `GarrysMod\bin\win64\`                    |
   | `garrysmod\cfg\zhphone_native.cfg`         | `GarrysMod\garrysmod\cfg\`                |

   Not found? `install.bat "D:\SteamLibrary\steamapps\common\GarrysMod"` — or copy the
   `garrysmod` and `bin` folders into your `GarrysMod` folder by hand.
3. Start the game and run once in the console: `exec zhphone_native`

**Server:** only `garrysmod/addons/zcity_phone` (no DLL; fix63 on the server relays server-side sounds to the phone's ear). Clients without the DLL still work
(Lua recorder: low resolution, no sound).

## Requirements for the native recorder

- 64-bit Garry's Mod: Steam → Garry's Mod → Properties → Betas → `x86-64 - Chromium + 64-bit binaries`
- NVIDIA GPU with NVENC, default D3D9Ex renderer (no DXVK)
- Game sound: Windows 10 2004 (build 19041) or newer
- Microphone: Windows Settings → Privacy → Microphone → *Let desktop apps access your microphone*

## Console variables

| Variable | Default | Meaning |
|--------------------------|-----------------------------------------------------------------------------------------------------------------|---|
| `zhphone_audio`          | 1 | 0 = silent clips                                                                                                |
| `zhphone_audio_mic` | 1 | 0 off, 1 only while you talk in voice chat, 2 always                                                                 |
| `zhphone_audio_game` | 1 | 0 off, 1 Garry's Mod only, 2 + whole-PC sound on older Windows                                                      |
| `zhphone_audio_ear` | 1 | fix63: 0 game heard at your head (fix62), 1 at the phone + "what you hear" while it is near you, 2 only at the phone |
| `zhphone_audio_mic_gain` | 1 | microphone loudness (0–4)                                                                                       |
| `zhphone_audio_play` | 1 | sound when watching clips in the gallery                                                                            |
| `zhphone_rec_stats` | — | recorder + clip sound state after a recording                                                                        |

## Uninstall the native part

Close the game, delete `garrysmod\lua\bin\gmcl_zhphone_win64.dll` and the three
`bin\win64\av*-zhphone32-*.dll`.

## Source and licenses

`native_source/` — DLL source (`src/`), FFmpeg SDK, build scripts and logs (`build-info/`),
the audio and phone-ear tests (`tests/audio/`, `tests/ear/`), third-party sources and licenses. See `THIRD_PARTY_NOTICES.md`
and `garrysmod/addons/zcity_phone/ASSET_CREDITS.txt`.

**Status:** built and tested offline (Windows x64 cross-build, Linux tests with mocked audio
devices); **not tested in-game on Windows yet.**
