# What was measured

Every claim here comes from a run on real hardware: a BOSS GT-10 in Advanced
mode on an Apple Silicon Mac, macOS 27.0. No run panicked the kernel.

The runs are from 2026-09-17 to 2026-09-20 and predate the review fixes of
2026-09-20. Those fixes leave the engine untouched, so the streaming numbers
still hold. They do change acquisition and stream start, so the hotplug results
need a new run.

## Streaming

| Run | Result |
|---|---|
| Capture, 12 s | 3009 transfers, 530788 frames, no lost packet, nothing out of order, no ring drop |
| Measured rate | 44100.03 Hz against the bus frame clock, 1 ppm high |
| Completion latency | 0 bus frames after the last frame of a transfer |
| Audio | peak -22.3 dBFS while playing, against a -73 dBFS noise floor |
| One output transfer | 4 frames, status 0, 264 of 264 bytes sent |
| Tone, 6 s | 1523 transfers, 268657 frames, no frame error, packets of 264 and 270 bytes |

`GetBusFrameNumberWithTime` returns mach absolute time. No run rejected an
observation as implausible.

## The installed plug-in

- The device appears as `BOSS GT-10`, 2 in and 2 out, in `system_profiler` and
  to `ffmpeg -f avfoundation`.
- Opening it switches IF0 and IF1 to alt 1. Closing it restores alt 0. Both
  were read back from the IO registry.
- ffmpeg recorded real audio through CoreAudio: peak -25.6 dBFS, 99.9 percent
  of frames not zero.
- The counters of the plug-in over one recording: clock source `device`, 694
  input reads, 0 silent, 512 valid frames in every read, read position 1512
  frames behind the capture end, 0 ring drops.
- Playback wrote 1377 buffers at positions that track the clock.
- With no app recording, the capture ring fills and drops, by design. The
  stream and the clock are unaffected.
- A 281 second recording: 26538 input reads, 0 silent, 0 ring drops, level
  steady within 2 dB. The longest run of digital zero was 64 frames, which is
  the noise gate of the pedal. A driver gap would span a whole 512 frame buffer
  and would have counted as a silent read.

## Unplug and replug

An unplug during a recording: the clock leaves the device and input reads
return silence. The interfaces drop to alt 0, and the recording app stops. A
replug reacquires the pedal, and the next recording streams at once.

Two defects showed up only here, and the hardware was the only way to find
them:

- The removal path tried to restore alt 0 on a device that was already gone.
  That failure set the quarantine meant for an unsafe teardown. The driver then
  held the pedal and refused to stream for the rest of the process.
- The engine faults before the removal notification arrives, because its
  transfers fail first. That is why quarantine has two kinds. See AGENTS.md,
  under Invariants.

An app that records when the pedal goes away can hang. ffmpeg does, through
AVFoundation, and it never exits. Two hung clients then delayed system sleep by
30 seconds each, which the power log blamed on them by name.

## Sleep and wake

The Mac slept and woke many times across 36 hours, in deep idle sleep and in
maintenance sleep. The driver logged no removal in that window. The pedal
stays attached across sleep, and the driver keeps both interfaces. `coreaudiod`
never restarted. A recording after the last wake streams at once. It ran on the
device clock, with alt 1 on both interfaces and 512 valid frames per read. There
was no lost packet, no silent read, and real audio at the noise floor.

This covers an idle sleep only. Sleep while a stream runs is not tested. The
bus suspends under transfers that are already queued, which is the one case
that can reach the kernel.

## Acquisition after a coreaudiod restart, 2026-09-20

Installing the driver restarts `coreaudiod`, which starts a new plug-in host
while the previous one still holds the USB interfaces. The new host found the
device and failed to open either interface six times over three seconds, then
gave up:

```
found device 0582:00da
did not open both audio interfaces (out 0x0, in 0x0)
giving up acquisition after attempt 5
```

Acquisition then waits for an arrival notification. The pedal was never
unplugged, so none came, and the device stayed published and dead until a
replug. The driver now also tries to acquire when a client asks for audio,
which covers this case. Verified: after the next install, a recording started
the stream with no replug, on the device clock from the first read.

## Levels, 2026-09-20

`USB/DGT Out Lev` at 200 percent against the same patch and playing style:

| `USB/DGT Out Lev` | Peak | Mean |
|---|---|---|
| Earlier setting | -22.3 dBFS | -36.3 dBFS |
| 200 percent | -7.9 dBFS | -26.5 dBFS |

The driver applies no gain in either direction, so the level is the pedal's.

## Input latency, 2026-09-20

The input safety offset moved from 1024 frames to 256. Measured as the distance
between the capture end and the position the HAL read from, while recording:

| Input safety offset | Read position behind the capture end |
|---|---|
| 1024 frames | 1817 frames, 41.2 ms |
| 256 frames | 1148 frames, 26.0 ms |

Both runs had no silent read, no ring drop, and no stray position. What remains
is mostly the buffer size of the app.

## Load conditions

`AudioServerPlugIn_LoadingConditions` gates loading only. With it narrowed to
`0582:00DA`, the plug-in still loads and keeps publishing the device while the
pedal is unplugged. The device reports `DeviceIsAlive` 0 in that state.

## What the HAL needs

A custom property must be declared in
`kAudioObjectPropertyCustomPropertyInfoList` and marshalled as a CFString or a
property list. A raw struct returns `kAudioHardwareUnknownPropertyError`.
