# ac3spdif-win

Dolby Digital Live for Windows: captures all system audio, encodes it to AC-3
or DTS in real time, and bitstreams the result out an S/PDIF interface as an
IEC 61937 data burst — so a receiver decodes 5.1 itself instead of being
handed stereo PCM.

Sound cards used to ship this as "Dolby Digital Live" or "DTS Connect", and
then mostly stopped. This is the same job done in software, generic over any
S/PDIF interface whose driver accepts an IEC 61937 exclusive format — and
falling back to a PCM carrier for the ones that do not.

It is a port of [ac3spdif](../ac3spdif) for macOS, with the pipeline rebuilt
on what Windows does better. The differences are not cosmetic:

| | macOS | Windows |
|---|---|---|
| Capture | BlackHole virtual device (third-party kernel driver) | **WASAPI loopback** — built into the OS, nothing to install |
| Claiming the device | hog mode + physical format switch + mixing property, each restored on exit | **one exclusive-mode Initialize**; releasing the client restores everything, so even a crash cannot leave the device stuck |
| DTS carrier | none — CoreAudio has a stream format for AC-3 alone, DTS hides in PCM | **`KSDATAFORMAT_SUBTYPE_IEC61937_DTS`** — first-class, non-audio bit set |
| Where the encoder runs | ffmpeg subprocess, feeding a ring of encoded bursts | **in-process, on the output device's own render thread** — no burst ring at all |
| Output buffer | IOProc locked to one whole burst: 32 ms | **event-driven at the device period: 10 ms** |
| Slack between the clocks | whole 32 ms bursts, plus a spare for the sawtooth | **PCM, adjustable to the sample** |
| PCM carrier integrity | float conversion on the way to hardware; dither cannot be ruled out | exclusive mode bypasses the mixer, bytes are **bit-exact by construction** |

Measured end to end on the same hardware (Sound Blaster Omni Surround 5.1),
real loopback capture, AC-3:

```
                       macOS        Windows, v1          Windows, now
                       (reference)  (subprocess + ring)  (render-thread encode)
capture buffer         10.7 ms      10 ms                10 ms
slack                  64 ms        64 ms (2 bursts)     15 ms (PCM margin)
encoder frame          32 ms        32 ms                32 ms
encoder delay          —            (unreported)         5.3 ms
output buffer          32 ms        10 ms                10 ms
                       ~139 ms      116 ms  at floor      72.4 ms at 15 ms
                                    244 ms  default       87.3 ms default (30 ms)
```

DTS drops the frame to 10.7 ms and has no encoder delay, so the same terms
come to about **46 ms at a 15 ms margin**. Self-test, which has no capture and
so no margin, measures 47.3 ms for AC-3 and **20.7 ms for DTS**.

## Two ways to run it

**Tray app** — `build\AC3SPDIFApp.exe`. Lives in the notification area.
Start/stop, pick devices and settings from the menu, and watch margin, drift,
dropout and encode-time figures live in the status window. Quitting stops the
stream; exclusive mode releases the device with it.

**Command line / background service** — `build\ac3spdif.exe`, or the app's
*Run as Background Service* item, which writes a Run-key entry so the same
binary streams headless from login with no UI at all.

The two are mutually exclusive by nature: whichever claims the output device
exclusively first wins. The app checks for a running service before starting
and offers to stop it, rather than leaving you to decode an
`AUDCLNT_E_DEVICE_IN_USE`.

Both drive the same engine, so there is one pipeline implementation, not two
that drift apart. The CLI and the app are each a couple hundred lines of
front end over it.

### The menu

| Item | What it does |
|---|---|
| **Start / Stop** | Begins or ends the bitstream |
| **Status Window…** | Live margin, drift, dropout, encode-time and burst figures, per-channel meters, and the log |
| **Capture From** | Which render endpoint to tap in loopback — normally *Default output* |
| **Output Device** | Where the bitstream goes. Each entry is annotated `✓ IEC 61937`, `~ PCM carrier only` or `✗ unsupported`, so the choice is informed rather than a guess from the name. *Automatic* picks the best IEC 61937-capable device |
| **Channels** | Capture width. *Automatic* matches the Windows speaker layout; `8-channel source → 5.1` is labelled that way because AC-3 has no 7.1 mode |
| **Codec** | AC-3 or DTS, with the frame length that drives latency |
| **Bitrate** | Per codec; 640k is the most AC-3 allows |
| **Carrier** | IEC 61937 or the PCM fallback |
| **Latency** | The PCM margin in milliseconds, with the floor for the selected devices shown |
| **Self-Test Tones** | Replaces capture with a distinct tone per channel |
| **Run as Background Service** | Installs the Run-key entry and starts the service now |
| **Start Streaming When App Opens** | Auto-start; does not launch the app itself |

Changing any device or format setting while streaming restarts the engine,
since the configuration is fixed when the pipeline starts. That is
deliberate: leaving the menu showing one thing while the stream does another
is worse than a half-second gap.

## How it works

```
 all system audio
       │
       ▼
 default output ──WASAPI loopback──▶ pcmRing ──▶ drift · resample · encode · IEC 61937 ──▶ S/PDIF
 (render clock A)                    margin      on the output's render thread,          (render clock B)
                                                 pulled one device period at a time
```

Four things make this work, and each is load-bearing:

**Capture is WASAPI loopback of a render endpoint.** Windows will hand any
client exactly what it is mixing to a device, in the device's mix format —
set the speaker configuration to 5.1 in Sound settings and loopback delivers
5.1 float. That configuration step is the same one BlackHole needed on macOS
("Configure Speakers"); the kernel driver it needed is simply gone. One
consequence to design around: loopback taps the shared mixer and exclusive
mode shuts that mixer out, so **the capture source and the bitstream output
cannot be the same endpoint**. Set the Windows default output to the device
you nominally listen through, and point the bitstream at the S/PDIF
interface; the engine refuses the degenerate case with an explanation rather
than a WASAPI error code. The tapped device keeps playing what it plays — if
you want sound only from the receiver, make the default output one with
nothing connected to it.

**The encoder runs on the output's render thread, pulled.** Each render event
asks for exactly one device period of bytes. The encoder hands them out of
the burst it is part way through sending, and encodes the next frame —
resample, correct, encode, frame as IEC 61937 — only when that runs short. So
the only encoded audio in flight is the rest of one burst, which is that
burst being transmitted rather than a buffer, and the only slack in the whole
pipeline is a margin of PCM ahead of the encoder. That is the placement the
macOS README identified as the way below its floor ("encode in-process and
drive the encoder from the output callback"), and it is what
[virtual-ac3-encoder](https://github.com/strepto42/virtual-ac3-encoder)
does; this implementation keeps the 10 ms device period rather than sizing
the endpoint buffer to a whole burst, and keeps bidirectional drift
correction rather than trimming only. Encoding costs **0.25 ms per 5.1 AC-3
frame (0.08 ms for stereo) and 0.43 ms per 5.1 DTS frame** on the render
thread, measured, out of a 10 ms period; the status window reports the
average and worst case, so that stays a reading rather than a claim.

**The output format is IEC 61937, in exclusive mode.** Windows defines
exclusive-mode formats that mean "this is an encoded burst, not audio" —
`KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL` and, unlike CoreAudio,
`..._IEC61937_DTS` too. Exclusive mode bypasses the audio engine entirely, so
no volume scaling, resampling or mixing can touch the burst, and the driver
raises the IEC 60958 channel-status *non-audio* bit that tells a receiver to
route the stream to its decoder rather than its DACs. Drivers disagree about
the format's spelling, so both the modern extensible shape and the legacy
`WAVE_FORMAT_DOLBY_AC3_SPDIF` tag are probed — the driver answers, not a
guess.

**Clock drift is corrected in the PCM domain, by a PI loop.** The capture
endpoint and the S/PDIF endpoint run off different oscillators, and a
bitstream cannot be resampled — every byte of a burst is load-bearing — so
each frame is built from a sample more or fewer than nominal: drop one to
shed, repeat one to pad. A dropped or duplicated sample at 48 kHz is
inaudible. The controller measures the margin at the moment each frame is
taken and keeps the trough over about a second, since the trough is what
decides whether a frame starves. The proportional term removes that error
over the next second, spread evenly; the integral term learns the clock error
itself and feeds it forward continuously. Proportional-only control — the
first version of this — settled a whole window's drift off target, measured
at 3 ms under a 3000 ppm clock error, and on the slow side that is margin
lost; with the integral term the trough holds within 0.1 ms of target at
±3000 ppm. The learned rate is kept across pauses, since it is a property of
the two crystals rather than of the audio.

When the margin runs dry the encoder sends encoded *silence* — the receiver
holds its Dolby lock — and resumes only with the whole margin rebuilt, so it
does not starve again on the next packet. Loopback delivers nothing at all
when nothing is playing, so this is also what idle looks like; an episode that
ends within a quarter second was a gap in playing audio and is counted as a
**dropout**, one that lasts longer was playback stopping and is not. Start-up
is the same mechanism: output begins at once, silence until the margin fills,
no special case and no timeout.

The engine also plays the Windows scheduling cards the macOS version had no
equivalent for: the capture and render threads run under MMCSS "Pro Audio",
loopback is drained on a high-resolution waitable timer (loopback streams do
not signal events — a documented WASAPI limitation), and the process holds a
1 ms timer resolution for the life of a run so short waits mean what they
say. A device that disappears mid-stream (`AUDCLNT_E_DEVICE_INVALIDATED`)
fails the engine cleanly with the reason, instead of leaving a render loop
spinning on a dead client.

## Requirements

- Windows 10/11 with any render endpoint whose driver accepts an IEC 61937
  exclusive format (check with `audioprobe`), or 2ch/16-bit exclusive LPCM
  for `--carrier pcm`
- To *build*: Visual Studio 2022 Build Tools (C++ workload) — no CMake — and
  MSYS2 (`winget install MSYS2.MSYS2`), which the FFmpeg build needs for its
  POSIX shell

Nothing needs installing to *run* it. The four FFmpeg DLLs ship beside the
binaries, and the binaries link the CRT statically.

## Setup

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-ffmpeg.ps1  # once; needs MSYS2
powershell -ExecutionPolicy Bypass -File scripts\build.ps1         # everything -> build\, runs the tests
```

To produce distributables from that:

```powershell
scripts\package-release.ps1     # dist\ac3spdif-win-<v>.zip - portable, unzip and run
scripts\build-installer.ps1     # dist\ac3spdif-setup-<v>.exe - needs Inno Setup
```

The installer (`winget install JRSoftware.InnoSetup` to build it) installs the
way Windows apps install: into Program Files by default — or per-user without
admin rights, the wizard offers both — with a Start Menu entry, optional
desktop and start-with-Windows shortcuts, an optional PATH entry so
`ac3spdif` and `audioprobe` work from a terminal, and a real uninstaller. The
uninstaller also removes the login-service Run-key entry the app may have
written while it ran, and deliberately leaves the settings under
`HKCU\Software\ac3spdif`, so a reinstall finds the devices and buffer sizes
it had before.

The setup exe is not code-signed, so SmartScreen will interpose on a
downloaded copy ("Windows protected your PC") — the same wall Gatekeeper puts
in front of the unsigned macOS app. *More info → Run anyway*, or build it
yourself; a user who hits that screen with no explanation reasonably
concludes the download is hostile, which is why it is explained here.

Then either:

```powershell
.\build\AC3SPDIFApp.exe        # tray app
.\build\ac3spdif.exe --list    # or start from the CLI
.\build\ac3spdif.exe --selftest --output "SPDIF"
```

`--selftest` is the thing to try first. It skips capture entirely and
synthesises a different pitch per channel on the render thread, which checks
the whole output path — exclusive claim, format negotiation, render events,
encoder, receiver handshake, channel order — without touching whatever is
playing. Your receiver should display "Dolby Digital" and each speaker should
sound a different note.

For 5.1 capture, one manual step, the exact analogue of the macOS "Configure
Speakers" step: **Sound settings → your default output device → set the
speaker setup to 5.1.** The mix format is what loopback capture delivers, and
it follows that setting. `audioprobe` reports it as `mix format`.

## Tests

`build.ps1` builds and runs `build\ac3spdif_tests.exe`, which needs no audio
hardware:

- **Round trip**, per codec: self-test tones through the in-process encoder,
  framed as IEC 61937, decoded back with FFmpeg's spdif demuxer. Every burst
  must carry the Pa/Pb sync words, and a Goertzel filter per decoded channel
  must find that channel's own pitch — so channel order is checked, including
  the 5.1-back to 5.1-side remap DTS needs, not just "it decodes".
- **Drift**: a simulated capture clock at 0, ±300 and ±3000 ppm against a
  simulated output clock, minutes of it in seconds. The margin must converge
  within 1.5 ms of target, the correction must match the clock error, and
  there must be no dropouts.
- **Starvation**: a 2 s pause counts as idle, not a dropout; an 80 ms capture
  stall mid-playback is exactly one dropout; a 20 ms one is absorbed.

## Working with other S/PDIF interfaces

Nothing here is specific to one device. `--list` and `audioprobe` rank every
render endpoint:

- **`✓ IEC 61937`** — the driver takes the encoded exclusive format for the
  selected codec. Nothing else to do.
- **`~ PCM carrier only`** — no encoded format, but bursts can ride in
  16-bit exclusive LPCM with `--carrier pcm`. The non-audio bit is not set,
  so the receiver must recognise the burst preamble itself — which is how DTS
  audio CDs have always worked. Exclusive mode bypasses the mixer, so unlike
  the macOS PCM carrier there is no float conversion and no dither risk; the
  bytes are bit-exact by construction.

Auto-selection only ever picks a `✓` device. A PCM carrier has to be named
with `--output`, deliberately: an AC-3 burst played into an analog output is
full-scale digital noise, and Windows' form factor for an endpoint
(`Speakers`, `S/PDIF`, `HDMI`) is a hint, not a guarantee. That is a choice
to make by hand, not one to make automatically.

Any device period works. The encoder hands out bytes, not bursts, so a driver
whose alignment rules force a period that is not a multiple of the burst
length cannot push the render loop into padding between bursts — which would
break the IEC 61937 burst spacing, and is the failure mode of sizing the
endpoint buffer to whole bursts.

**Vendor software that does its own passthrough will hold the device.** The
Sound Blaster Omni this was verified on ships Creative software whose encoder
claims the S/PDIF endpoint exclusively at login; until it was disabled, every
start failed with "already claimed in exclusive mode by another process".
If that error appears with no obvious process to blame, look for the sound
card's own control panel first.

## The bundled FFmpeg

The engine links four FFmpeg libraries, shipped as DLLs beside the binaries:
`avcodec` (the AC-3 and DTS encoders), `avformat` (the IEC 61937 `spdif`
muxer), `swresample` (mix format to encoder format, layout and rate) and
`avutil`. It is not a stock build: `scripts\build-ffmpeg.ps1` configures with
`--disable-everything` and switches back on only those encoders and that
muxer, plus the matching decoders and demuxer that the tests use to prove
the output decodes back. The four DLLs come to about 2.5 MB together.

They are built with MinGW, so the script generates MSVC import libraries from
each DLL's own export table, and it links the MinGW runtime into them
statically — then checks each DLL's imports and refuses to finish if any
needs a non-system DLL. That check earned its place on the first run:
`avutil` still wanted `libwinpthread-1.dll` for `clock_gettime`, which would
have loaded fine here and failed on any machine without MSYS2.

**Licensing.** The build enables neither `--enable-gpl` nor
`--enable-nonfree`, so it stays LGPL-2.1+. The binaries link the DLLs
*dynamically*, and the DLLs ship as separate, replaceable files — the LGPL's
own condition for linking. `build-ffmpeg.ps1` writes `vendor\FFMPEG-NOTICE.md`
with the exact version and configure flags, and `build.ps1` copies it and the
LGPL text beside the DLLs, because the obligation is for them to travel with
the libraries rather than merely to exist in the repository they were built
from. Ship them if you redistribute.

The subprocess the first version used bought a fault boundary: on macOS a
crash while holding hog mode left the device stuck until replugged. On
Windows exclusive mode releases the device when the process dies anyway, so
that boundary protected very little, and it cost two pipes, three threads and
a ring of whole bursts.

## Latency

The PCM margin is the only knob. Everything else is a device's own buffer or
inherent to the format. `--buffer <ms>` sets it, to the sample; the app has a
**Latency** submenu.

The CLI prints the breakdown at start-up and again, measured, at exit. Every
term is a real reading — a device's own period, a figure libavcodec reports,
or the measured margin — not an estimate:

```
latency : capture buffer        480 frames     10.0 ms   engine period, loopback
latency : pcm margin            722 frames     15.0 ms   what --buffer controls
latency : encoder frame        1536 frames     32.0 ms   inherent to the format
latency : encoder delay         256 frames      5.3 ms   codec look-ahead
latency : output buffer         480 frames     10.0 ms   exclusive endpoint buffer
latency : TOTAL                3474 frames     72.4 ms   end to end
```

The margin has to cover one loopback capture period — 10 ms on every device
measured — plus scheduling jitter, which is where the 15 ms floor the engine
works out comes from. The floor only depends on the codec when the output
device's period is longer than a burst, since then one render event needs
several frames at once; the old burst ring needed four DTS bursts where two
AC-3 bursts sufficed, because its unit was the burst. The engine warns at
start-up when the margin is below the floor and does not forbid it: measured
here, even 10 ms ran 40 s with no dropouts, but on a busy machine it will
not.

| `--latency` | margin | AC-3 total, measured |
|---|---|---|
| `minimum` | 15 ms | 72 ms |
| `low` | 20 ms | 77 ms |
| `balanced` | 25 ms | — |
| `safe` (default) | 30 ms | 87 ms |
| `maximum` | 80 ms | — |

One measured Windows-specific caveat, recorded in `BitstreamOutput.cpp`: the
render stream runs at the device's *default* period, not its minimum. The
Omni advertises a 144-frame minimum and accepts an Initialize at it, then
paces render events at its 480-frame default anyway — so a loop filling 144
frames per event drains at 30% of real time and the wire starves. The default
period is the one drivers demonstrably honour for a passthrough format.

Watch the `dropouts` figure: if it climbs while audio plays, the margin is
too small for that machine. `margin` should sit on its target; `drift` is the
correction rate in ppm and only matters if it approaches the `--max-drift`
ceiling (8 samples per 1536, about 5200 ppm). `encode` is the average and
worst time the render thread spent encoding a frame, against a 10 ms period.

## Codecs

`--codec ac3` (default) or `--codec dts`, and a **Codec** submenu in the app.

| | AC-3 | DTS |
|---|---|---|
| Frame | 1536 samples | 512 samples |
| Burst | 6144 bytes | 2048 bytes |
| Frame duration | 32 ms | **10.7 ms** |
| Encoder delay | 5.3 ms | 0 |
| IEC 61937 data type | 1 | 11 |
| Default bitrate | 640k | 1509k |
| Offered bitrates | 192k–640k | 754k–1509k |
| Carrier | IEC 61937 or PCM | IEC 61937 or PCM |

The frame is the largest fixed term in the budget, so DTS takes about 26 ms
off end-to-end latency at any margin. On macOS the DTS row read "PCM only";
Windows has an encoded format for both, which removes DTS's biggest caveat
there. What remains true on both systems:

- **The bitrates do not overlap.** FFmpeg's DTS encoder refuses anything at
  or below 640k for 5.1 — which is every AC-3 setting — and fails to open
  rather than degrading. A bitrate the codec cannot take is replaced with the
  codec's default and logged; the app's menu only offers what the selected
  codec accepts, and switching codec clears a stale value.
- **1509k is the ceiling the burst imposes**, not the encoder: a Type I burst
  is 2048 bytes with an 8-byte preamble, and 1536k needs all 2048, whereupon
  the muxer emits a stream with no sync pattern at all — silently unusable,
  so it is not offered. 1411k, the rate DTS discs carry, is the safer choice
  for a receiver of unknown temperament.
- **FFmpeg's DTS encoder is experimental** and is opened with experimental
  compliance; it takes `5.1(side)` rather than AC-3's back-channel `5.1`, so
  the layout is stated per codec and the resampler does the remap.
- **It encodes LFE about 4.6 dB low.** A full-scale test tone on the LFE
  channel decodes at −21.1 dBFS against −16.5 on the other five. That is the
  encoder, not this pipeline: the stock `ffmpeg` CLI produces the identical
  figure, to six decimal places, from the same signal.

## Diagnosing a silent speaker

```powershell
.\build\ac3spdif.exe --output none --meters --status 0
```

`--meters` prints one level bar per channel the *mix* presents, not just the
ones being encoded. A channel marked `!` is beyond the capture width and is
being discarded, so signal appearing there is the answer to "why is that
speaker silent" — and the engine says so outright, once, when it happens.
`--output none` encodes and throws the bitstream away, pulling at the real
wire rate; it claims no device at all, so capture can be examined while
something else is using the hardware.

The meter decays; the summary printed on exit is the undecayed peak per
channel over the whole run, so nothing has to be caught live. The app shows
the same meters continuously in the status window.

If a channel that should carry audio reads silent everywhere: the mix format
is stereo. Set the device's speaker configuration to 5.1 (see Setup), and
note that applications bind their channel layout when they open the device,
so restart the player after changing it.

`--dump <file>` tees the IEC 61937 stream to a file, and
`ffmpeg -f spdif -i <file>` decodes it straight back to PCM — which cleanly
separates a capture or encode fault from a cabling or receiver fault. The
bundled DLLs are not a command-line ffmpeg; any full build, such as
`winget install Gyan.FFmpeg`, does the decoding. `--duration <seconds>` stops
a run cleanly, for scripts.

## Verified

On a Sound Blaster Omni Surround 5.1 (S/PDIF form factor, IEC 61937 AC-3 and
DTS accepted in both format spellings at 48 kHz), Windows 11 Pro 26200:

- **Self-test → S/PDIF, AC-3**: `48000 Hz IEC 61937 AC-3 2ch 16-bit`, 375
  bursts in 12.0 s — exactly 31.25/s — zero dropouts, 47.3 ms end to end,
  encoding at 0.25 ms average and 0.35 ms worst on the render thread.
- **Self-test → S/PDIF, DTS**: `IEC 61937 DTS 2ch 16-bit`, 1127 bursts in
  12.0 s (93.9/s against a theoretical 93.75), zero dropouts, 20.7 ms end to
  end, 0.43 ms average and 0.81 ms worst encode.
- **The bitstream itself**: both dumps decode through an independent,
  full FFmpeg as 5.1 at the configured rate, every channel at −16.48 dBFS —
  the exact test amplitude — bar DTS's LFE, explained above.
- **Real loopback capture → S/PDIF**: a Bluetooth headset (2 ch, 48 kHz) as
  the Windows default output, captured and bitstreamed as AC-3 2.0 for 40 s
  at each of 30, 20, 15 and 10 ms margin. Zero dropouts at every setting, the margin
  held within 0.2 ms of target, and the controller settled on the ~−100 ppm
  offset between the Bluetooth stack's clock and the Omni's.
- **Earlier, on the subprocess design**, the Omni's own 96 kHz 6-channel
  Speaker mix was captured, resampled and encoded with the test tone metered
  on exactly FL/FR; that path — mix rate differing from the wire rate — now
  runs through libswresample in-process, and is covered by the same code.
- **Format probing**: the extensible and legacy format spellings both
  accepted on the Omni; HDMI (NVIDIA) and virtual endpoints correctly
  reported as PCM-carrier-only; ranking picks the S/PDIF port automatically.
- The tray app launches and enforces its single instance. Its menus are
  built but were not exercised by a human in this pass, and no receiver was
  attached to the S/PDIF port — everything upstream of the cable is
  confirmed correct, the handshake itself is not.

## Licence

MIT; see `LICENSE`. The bundled FFmpeg libraries are licensed separately
under the LGPL-2.1-or-later and are linked dynamically; see
`FFMPEG-NOTICE.md` for the exact build and the corresponding source.
