# GT-10 audio driver

A CoreAudio driver that records and plays audio through the BOSS GT-10 on Apple
Silicon. The pedal appears as **BOSS GT-10**, 2 in and 2 out, 24 bit, 44100 Hz.

Roland's own driver is x86 only and needs a kernel extension, so it cannot run
on an Apple Silicon Mac. This one is an AudioServerPlugIn. It runs in user
space with an ad-hoc signature. It needs no Apple developer account and no
change to System Integrity Protection.

## Requirements

- An Apple Silicon Mac, verified on macOS 27.0.
- The Xcode command line tools.
- The pedal in Advanced mode.

To select Advanced mode, open `SYSTEM MENU` > `USB` > `Driver Mode` on the
pedal. Then power the pedal off and on. Advanced mode is 24 bit and carries
MIDI. Standard mode is a different USB device that macOS already drives, at
16 bit.

## Install

```
git clone <this repository>
cd gt10-audio
make driver-install
```

The command asks for your password, because the plug-in goes into a system
directory. It then restarts `coreaudiod`, which cuts all audio on the Mac for a
moment.

To remove the driver again:

```
make driver-uninstall
```

The driver claims the two audio interfaces of the pedal while the pedal is
attached. It streams them while an app has the device open. MIDI stays
available to other software, because the driver never touches the MIDI
interface of the pedal.

## Check that it works

```
make driver-check       # install, then report whether the plug-in loaded
make hal-probe          # counters from inside the running plug-in
```

Run `make hal-probe` while an app records. It prints one line:

```
clock device seed 10 | reads 1078 silent 0 lastPos 196096 valid 512 | ...
```

- `clock device` means the pedal drives the timeline. `clock synthetic` means
  the pedal is absent or its stream did not start, and the audio is silence.
- `silent` counts input reads that found no audio. This number must stay at 0.
- `dropped` counts captured frames that no app read. It grows while only
  playback runs, which is correct.

## Levels

The driver passes samples through unchanged in both directions, and it
publishes no volume control. The volume keys of the Mac and the menu bar slider
do nothing for this device. Only the fader of an app, and the pedal itself,
change a level.

Both level settings live on one screen of the pedal:

1. Press `SYSTEM`.
2. Select `INPUT/OUTPUT`.
3. Select the `TOTAL` screen, page 3.

| Setting | Range | What it changes |
|---|---|---|
| `USB/DGT Out Lev` | 0 to 200 percent | How loud the pedal sends audio to the Mac |
| `USB Mix Level` | 0 to 200 percent | How loud audio from the Mac arrives in the pedal |

`USB Mix Level` applies when `INPUT SELECT` is set to Guitar 1 to 3. The owner's
manual names `USB/DGT Out Lev` first under "volume too low".

`USB/DGT Out Lev` drives the DIGITAL OUT jack as well. The OUTPUT LEVEL knob
does not reach either of them: it acts on the OUTPUT and PHONES jacks only, so
it cannot change what you record. USB always carries the processed patch
signal, because the pedal offers no dry tap for it.

## Hearing the pedal through the Mac

macOS does not route an input to an output by itself, so an app has to do it.
For a quick test, with ffmpeg installed:

```
ffplay -f avfoundation -i ":1" -nodisp -af volume=12dB
```

The index comes from `ffmpeg -f avfoundation -list_devices true -i ""`. This
path buffers heavily, so it is a check and not a way to play. Stop it with
ctrl-c. A live input never ends, so `-t` and `-autoexit` do not stop ffplay, and
it holds the device until you do.

To play, use a DAW with input monitoring, such as GarageBand. Its track fader
gives the gain that this driver does not. The OUTPUT and PHONES jacks of the
pedal stay the path with no added latency.

## Limits

- The pedal offers 44100 Hz only in this mode.
- The driver reports 5.8 ms of input safety offset and 23.2 ms of output safety
  offset. The input figure was measured. The output one is bounded by the
  engine, which renders up to 31 bus frames ahead.
- The device stays listed while the pedal is unplugged, and reports itself not
  alive. A load condition gates loading, not unloading.
- An app that records when you unplug the pedal can hang. ffmpeg does. Quit it,
  because a hung client also delays system sleep by 30 seconds.
- If a stream cannot be stopped cleanly, the driver quarantines the device until
  `coreaudiod` restarts. Reusing memory that the kernel can still own is worse.

## Build and test

```
make check     # format, static analyzer, and every test suite
make test      # the suites alone, under AddressSanitizer and UndefinedBehaviorSanitizer
make help      # every target
```

The suites never touch the pedal, because a fake replaces the USB layer. The
hardware probes live in `AGENTS.md`, with the warning they need.

## More

- `docs/PROTOCOL.md` describes the USB and MIDI protocol of the pedal.
- `docs/VERIFICATION.md` lists what was measured on hardware.
- `AGENTS.md` is the working guide for this repository.

## License

MIT. See `LICENSE`.
