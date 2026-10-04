# AirPlay receiver

`components/airplay` receives AirPlay 1 (RAOP) audio: ALAC or L16, 44.1 kHz,
16-bit stereo, optionally AES-encrypted. AirPlay 2 is not implemented; keep the
RTSP session (`receiver.cpp`) and the RTP/playback side (`stream.cpp`) apart so
its control channel can replace the first without touching the second.

`AirPlayReceiverScreen` (Home > Network) is the only owner. The receiver
listens and advertises only while that screen is open and Wi-Fi has an address,
so it never competes with `Player` for `bsp_audio`. Opening it clears the Home
page stack, which also drops `WifiPage` and with it the single
`wifi::Manager` listener slot; the screen polls `wifi::manager().status()`
instead of taking the slot.

## Protocol facts the code depends on

- **Apple-Challenge.** The response is the AirPort Express RSA key's raw
  PKCS#1 v1.5 signature (no DigestInfo) over challenge ‖ local address of the
  RTSP socket (4 or 16 bytes) ‖ the MAC in the service name, zero-padded to 32
  bytes. The MAC only has to match the `<MAC>@<name>` instance name, so the
  board's base MAC is used rather than the radio's.
- **Sync anchor.** A sync packet carries `rtp_less_latency` (bytes 4–7) and an
  NTP time; frame `rtp_less_latency - 11025` is due at that NTP time. The
  11025-frame offset is what current senders expect on top of the latency the
  packet states (shairport-sync's `fixedLatencyOffset`).
- **Clock.** The receiver sends timing requests (every 300 ms at first, then
  every 3 s) and uses the offset from the lowest-RTT reply of the last eight.
- **Ports.** All sockets bind port 0 and the advertised port is whatever the
  OS gave: macOS's own AirPlay receiver holds 5000 and 7000.

## Playback

Packets are decrypted and decoded on arrival into a 512-slot PCM ring
(about 720 KB of PSRAM for 352-frame packets), so resent packets can land out
of order. The player opens the output once the first packet is less than
100 ms from due, writes silence up to that point, then writes one packet per
call to `airplay::Output::write`. The output is the app's (`bsp_audio_*` in
`AirPlayReceiverScreen`); the component only relies on `write` blocking while
the output is full.

The time between "now" right after a write returns and when the next packet is
due is constant while the clocks agree; the output's own depth is folded into
that constant, so it never has to be known. The first 32 packets are ignored
while the output fills, a slow average sets the baseline at packet 256, and
each packet afterwards inserts or drops one frame while the average is more
than 2 ms off it. A faster average or a 1 ms threshold corrected on write-time
jitter alone. More than 50 ms off, or a second without packets, stops the
output and waits for the next anchor.

AirPlay volume (−30…0 dB, −144 mute) is applied as the same attenuation in dB:
`bsp_audio_set_volume` is linear in dB at 0.4 dB per step, so −30 dB is 25 and
mute is 0. The Sound setting is restored when the screen closes.

## Track info and artwork

Senders only send metadata to receivers that advertise `md=0,1,2`. It all
arrives as `SET_PARAMETER`:

- `application/x-dmap-tagged`: an `mlit` holding `minm` (title), `asar`
  (artist) and `asal` (album).
- `image/jpeg` or `image/png`: the artwork; `image/none` clears it. A body
  bigger than the 16 KB request buffer gets a PSRAM buffer of its own, up to
  2 MB, which then becomes the artwork's storage without a copy.
- `progress: start/current/end` in RTP time, sent at track start and on seeks
  only, so the position comes from the RTP time of the packet last written to
  the output.

The screen polls `airplay::now_playing()` for the text and times and gets the
artwork through the listener. It decodes the artwork on a task of its own
(16 KB PSRAM stack, as for any image_framework caller) with
`image_decode_to_fit()`, which unlike the file covers' path also enlarges:
iPhones send 512x512 artwork for the 552 px box. The fitted picture goes to the
start of framebuffer 1, like `AudioPlayerScreen`'s, and the rest of that
framebuffer is the intermediate, so nothing is allocated. A baseline JPEG that
has to grow is decoded 1:1 by the JPEG hardware alone (the pipeline's Layer 1
decoder, no PPA) and enlarged in one software resize; one that shrinks keeps
the covers' PPA path. The LVGL image showing the framebuffer is dropped before
the decode starts writing into it.

## Remote control

The sender's RTSP requests carry `DACP-ID` and `Active-Remote`. The remote
control service is `iTunes_Ctrl_<DACP-ID>._dacp._tcp` on the sender's own
address, so only its port is resolved over mDNS (`mdns_query_srv`, or
`DNSServiceResolve` on the simulator). Commands are
`GET /ctrl-int/1/<command>` with the `Active-Remote` header, sent from a task
of the component's own so the UI never waits on the network: `playpause`,
`nextitem` and `previtem`. A failed request drops the port and resolves it
again.

The volume row only shows the sender's volume. The last volume is kept past
the end of a session, as the output keeps it too: a sender that re-announces
does not always send it again. `setproperty?dmcp.device-volume`
is answered with 2xx but ignored by both iOS and current macOS Music, which no
longer has a volume of its own.

## Memory

The stream buffers and the three receiver tasks' stacks are PSRAM. mbedtls
(`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`) and mdns (`CONFIG_MDNS_MEMORY_ALLOC_SPIRAM`)
allocate from PSRAM as well.

## Simulator

The simulator advertises through the OS `dns_sd` API (mDNSResponder on macOS,
avahi's compat library elsewhere) and uses the host's mbedtls 4 from the flake
for PSA crypto and ffmpeg for ALAC. A sender on the LAN can stream to it the
same way as to the board.
