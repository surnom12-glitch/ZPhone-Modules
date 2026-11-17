fix74 / fix75 Cross Server Calls - native tests (Linux, no Windows / Steam needed)

  g++ -std=c++17 -O2 -Wall -o csctest csctest.cpp -lpthread && ./csctest      -> all ok
      csc_audio.h: IMA-ADPCM (SNR, bad / short frames refused), the 48 -> 16 kHz filter, the
      jitter buffer (30 ms jitter + 3 % loss, concealment, no clicks), 1 % clock drift, a slow
      sender, a 3 s gap, the uplink echo removal (37 / 160 / -6 ms, with a voice or game noise
      at this end kept)
  g++ -std=c++17 -O2 -Wall -o nettest nettest.cpp -lpthread && ./nettest      -> ALL OK
      csc_net.h against a fake Steam (3 players): publish, the list (not yourself, rate limit),
      call -> ring -> ringing -> answered, voice both ways with 20 % loss, big and small Lua
      messages, busy, hang up, decline, no answer, cancel while ringing, a RING from a stranger
      ignored, the other game gone (lost after 12 s), turned off -> gone from the list /
      unavailable, text clipping (control chars, length, UTF-8), SteamID parsing
      fix75 video (csc_vsend / csc_vtake): binary JPEG pictures split into packets + a parity
      packet, the newest complete picture wins, the biggest picture, 10 % packet loss (one lost
      packet per picture rebuilt from the parity), two lost = skipped, both directions, no call

csc_steam.h (the real steam_api backend) is only compiled by the Windows cross-build.
The Lua side: ../csc74_lua/ (README.txt there).

fix77 (protocol 2, end-to-end encryption)
  g++ -std=c++17 -O2 cryptotest.cpp -o cryptotest -lcrypto && ./cryptotest   # crypto vs RFC vectors + OpenSSL
  g++ -std=c++17 -O2 nettest.cpp -o nettest && ./nettest                     # 69 checks, "all ok"
  csc_crypto.h has no dependencies (Windows: BCryptGenRandom -> link bcrypt).
