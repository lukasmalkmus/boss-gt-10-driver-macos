# Working guide

A CoreAudio AudioServerPlugIn for the BOSS GT-10 in Advanced mode
(USB `0582:00DA`), on Apple Silicon. It owns the two vendor-class audio
interfaces of the pedal, runs an isochronous engine in both directions, and
publishes one device to the HAL.

Read `docs/PROTOCOL.md` before you change anything that talks to the device.
Read `docs/VERIFICATION.md` before you claim that something works.

## Rules

- Never run `make driver-install`, `make driver-uninstall`, or `make
  driver-check`. They need `sudo`, which only the human can give. Ask.
- Never run a probe that submits a transfer without approval for that one run.
  An isochronous transfer can panic the kernel. Name the failure mode first.
- Never disable System Integrity Protection.
- The pedal has one owner per interface. Uninstall the plug-in before a probe.
- A test that still passes with its fix reverted proves nothing. Mutate the
  code by hand, watch the test fail, then restore the file from a copy.

## Provenance

The code is MIT and must stay clean of anything that is not. Audited on
2026-09-20 against the two projects it could plausibly have borrowed from:

- `lnswlrd/MultiRolandDriver`, which is GPL-3. 21 of 2929 lines match, and all
  of them are the one canonical way to write an IOKit call, such as
  `req.bInterfaceClass = kIOUSBFindInterfaceDontCare;`.
- The Apple sample lineage, compared against BlackHole. 36 lines match, and
  every one is a `case` label naming an Apple constant.

No third-party source file is vendored. The only headers outside this tree are
system frameworks.

Facts taken from GPL sources are cited in `docs/PROTOCOL.md`: the Linux ALSA
quirk tables, and the FxFloorBoard address map. Facts about a device carry no
licence. Code does. Read those sources to learn what the pedal does, then write
the implementation here.

## Map

| Path | What it holds |
|---|---|
| `Driver/GT10Audio.c` | The plug-in entry points, the HAL contract, the real-time IO path, the clock epoch |
| `Driver/GT10USBDevice.cpp` | Device ownership, hotplug, the monitor that owns the stream |
| `Driver/GT10USBStream.cpp` | The isochronous engine, 4 transfers of 4 bus frames per direction |
| `Driver/GT10StreamCore.cpp` | Pure frame logic: capture extraction, frame status, playback pacing |
| `Sources/GT10Ring.c` | A lock-free ring, one writer and one reader |
| `Sources/GT10Clock.c` | Bus frame to host time, zero timestamps, a two-slot seqlock publisher |
| `Sources/USBAudioFormat.cpp` | UAC-1 descriptor parsing |
| `Tests/` | The suites, a fake USB layer, and the two hardware probes |
| `.ripwire_notes` | One line per hard-won fact, addressed by file |

## Build

```
make check     # fmt-check, the static analyzer, and every suite. This is CI.
make test      # the suites, under AddressSanitizer and UndefinedBehaviorSanitizer
make fmt       # rewrite the sources in the project style
make driver    # build the bundle, without installing it
```

Every compile runs with `-Wall -Wextra -Werror`. The static analyzer runs with
warnings as errors. Keep both clean.

## Hardware probes

The probes open the real device, so the plug-in has to be uninstalled first.
Ask before running one, and name the failure mode for that run.

```
make usb-probe PROBE_ARGS="--pipes"                 # read pipe properties only
make usb-probe PROBE_ARGS="--capture 10 out.wav"    # stream the input to a file
make usb-probe PROBE_ARGS="--play 5"                # a 440 Hz tone on the output
make hal-probe                                      # counters from the running plug-in
```

**WARNING**: An isochronous transfer can panic the kernel. One did on
2026-09-12, about 3 ms after submission. Treat every probe that submits a
transfer as a run that can reboot the Mac.

## Invariants

These cost a debugging session each. Do not undo one without reading why it
exists.

- **Write the data, then publish it.** On Apple Silicon an earlier atomic store
  can become visible to another core after a later plain store. The reverse
  order, an atomic announcement before the data, was measured broken on this
  hardware.
- **The real-time path allocates nothing, locks nothing, and waits for
  nothing.** `DoIOOperation` and everything under it obey this.
- **Every wait has a deadline.** A stuck engine is quarantined, never joined
  forever. The kernel can still own a transfer, so a quarantined engine keeps
  all of its memory.
- **One serial queue owns every USB step.** Acquisition, retry, stream start,
  stream stop, and release never overlap. Callbacks from IOKit arrive there too.
- **Quarantine has two kinds.** An engine that cannot be joined is permanent.
  An interface left on an unknown alternate setting belongs to the attachment
  and retires with it. The engine faults before the removal notification
  arrives, which is why the two are separate.
- **A stream request outlives an absent pedal.** An app can start recording
  while the pedal is unplugged, because the device stays listed. The monitor
  starts the stream when the pedal arrives.
- **Acquisition happens on arrival and on demand.** Retries give up after a few
  seconds. A restart of coreaudiod outlives that: the previous host still holds
  the interfaces and no arrival ever comes, because the pedal never moved. A
  client asking for audio is the second trigger.
- **A position from one timeline must not reach a ring built on the other.** A
  cycle can read its sample time before the clock moves and reach the ring
  after. The reader's floor only rises and the writer's end only moves forward,
  so one stray position silences the device for the session.
- **A faulted stream starts again, at most three times per request.** Nothing
  else would: the HAL never repeats StartIO, so a single late transfer would
  otherwise leave the device alive and silent.

## Tests

Six suites, about 4600 checks, all under both sanitizers. The plug-in suite
replaces the whole USB layer with `Tests/FakeUSB.c`, so it proves how the HAL
side answers the monitor. It does not prove the monitor itself. The monitor is
covered by injected operations in `Tests/USBDeviceTests.c` and by the hardware
runs in `docs/VERIFICATION.md`.

`Tests/usb_probe.c` and `Tests/hal_probe.c` are tools, not tests. They talk to
real hardware.
