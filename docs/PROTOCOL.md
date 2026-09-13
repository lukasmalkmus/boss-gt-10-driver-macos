# The GT-10 over USB

Read from the device on 2026-09-12, macOS 26.6.2 on arm64, `bcdDevice` 0100.
The isochronous facts were measured on 2026-09-17 on macOS 27.0.

## Two devices in one pedal

| Driver Mode | VID:PID | Device class | What macOS binds |
|---|---|---|---|
| Standard | `0582:00DB` | 0 (composite) | AppleUSBAudio, class compliant |
| Advanced | `0582:00DA` | 255 (vendor) | nothing |

The mode setting is `SYSTEM MENU` > `USB` > `Driver Mode`, and it takes effect
after a power cycle. This driver handles Advanced mode only.

## Standard mode descriptors

```
IF 0  class 1/1    AudioControl
IF 1  alt 1  class 1/2  AS out, EP 0x01 isoc adaptive, 192 B/frame
IF 2  alt 1  class 1/2  AS in,  EP 0x82 isoc async,    192 B/frame
format: 2 ch, 16 bit, 44100 Hz
```

## Advanced mode descriptors

```
IF 0  class 255/2/2   audio OUT
      alt 0  cs-iface 06 24 F1 01 00 00   (Roland marker, type 01 = audio)
      alt 1  EP 0x01 isoc adaptive, 288 B/frame
             AS_GENERAL 07 24 01 01 00 01 00
             FORMAT_I   0b 24 02 01 02 03 18 01 44 AC 00
IF 1  class 255/2/1   audio IN
      alt 1  EP 0x82 isoc asynchronous, 288 B/frame, same format
IF 2  class 255/3/0   MIDI
      alt 0  cs-iface 06 24 F1 02 01 01   (type 02 = MIDI, 1 in cable, 1 out cable)
             EP 0x03 BULK OUT 32 B
             EP 0x84 BULK IN  32 B
      alt 1  EP 0x03 bulk OUT, EP 0x84 INTERRUPT IN interval 1
format: 2 ch, 24 bit, 44100 Hz
```

Advanced mode is 24 bit and Standard mode is 16 bit, at the same sample rate.

Advanced mode has no UAC AudioControl interface. The 6-byte vendor
marker on IF 0 alt 0 stands where the AudioControl topology would be. That is
why `AppleUSBAudio` never binds, whatever the class byte says.

Mainline Linux does not drive this device either. `sound/usb/quirks-table.h`
holds 58 Roland entries and none of them is `0582:00DA`. The generic path
`create_auto_pcm_quirk()` in `sound/usb/quirks.c` accepts this exact shape: a
vendor-class interface with two or more alternate settings, an isochronous
endpoint on alt 1, and both `UAC_AS_GENERAL` and `UAC_FORMAT_TYPE` present. The
audio descriptors are ordinary UAC-1 behind a vendor class byte.

Alt 1 on IF 2 is the low-latency variant. ALSA switches to it in
`snd_usbmidi_switch_roland_altsetting` whenever alternate setting 1 carries an
interrupt endpoint.

## Pipes on alt 1

`GetPipePropertiesV3` reports this after `SetAlternateInterface(1)`:

| Interface | alt | dir | ep | type | sync | usage | interval | wMaxPacketSize |
|---|---|---|---|---|---|---|---|---|
| IF 0 | 1 | 0 out | 1 | 1 isoc | 2 adaptive | 0 data | 1 | 288 |
| IF 1 | 1 | 1 in | 2 | 1 isoc | 1 async | 0 data | 1 | 288 |

`bSyncType` is the `USBSpec.h` enum, not the raw `bmAttributes` bits. The device
is full speed, so one bus frame is 1 ms.

## What a transfer returns

A single `ReadIsochPipeAsync` on IF 1, requesting 8 frames at `frReqCount` 288
each, returns this:

- The submission and the completion both return `kIOReturnSuccess`.
- Every frame returns `frStatus` `kIOReturnUnderrun` (`0xE00002E7`) with
  `frActCount` 264, which is 44 sample frames. A short packet reports an
  underrun, not a success.
- The buffer is slotted. Frame `i` starts at offset `i * 288`, and the bytes
  after `frActCount` in each slot stay untouched. The buffer is not packed.
- The data is 24-bit little-endian stereo.

An asynchronous IN transfer must request the full 288 bytes per frame. The
device decides how much it sends. A request paced to 264 or 270 bytes loses
samples.

## MIDI

The pedal speaks standard USB-MIDI 1.0, in 32-bit event packets. The high
nibble of byte 0 is the cable number and the low nibble is the code index
number. The pedal shows no Roland deviation. ALSA agrees: the `QUIRK_MIDI_ROLAND` path
in `sound/usb/midi.c` keeps `snd_usbmidi_standard_ops` and replaces endpoint
discovery only, by reading the `06 24 F1 02 <in> <out>` descriptor shown under
Advanced mode descriptors.

A round trip over IF 2, claimed from user space with libusb, needs no kernel
extension and no entitlement. Both directions work:

```
TX  F0 7E 00 06 01 F7                       Identity Request, device ID 00
RX  F0 7E 00 06 02 41 2F 02 00 00 00 00 00 00 F7
                        ^^ Roland  ^^^^^ family 2F 02

TX  F0 41 00 00 00 2F 11 00000000 00000010 <cks> F7    RQ1
RX  F0 41 00 00 00 2F 12 00000000 08 00 ... 78 F7      DT1, 16 data bytes
```

The device ID must be `00`. A broadcast Identity Request to `7F` gets no answer.

The SysEx frame:

```
F0 41 <dev=00> 00 00 2F <cmd> <addr 4> <data...> <cks> F7
cmd 11 = RQ1 (request), 12 = DT1 (data set)
cks = (128 - (sum(addr + data) & 0x7F)) & 0x7F
```

The model ID and the address map come from the FxFloorBoard `gt-10` branch,
in `globalVariables.h` and `midi.xml`.

## Roland's own macOS driver

`https://static.roland.com/assets/media/tgz/gt10_m13d103.tgz` is version 1.0.3
from 2017. It holds four payloads:

- `RDUSB00DADev.kext`, x86_64, IOAudioFamily and IOUSBHostFamily, matched on
  `idProduct` 0xDA. All USB logic lives here.
- `RDUSB00DAMidi.plugin`, x86_64 and i386, a CoreMIDI CFPlugIn. It links
  CoreFoundation, IOKit and CoreMIDI only, and holds no USB code. It reaches the
  kernel extension through `IOConnectCallScalarMethod` on
  `jp_co_roland_RDUSB00DADev_KextUserClient`.
- `RDUSB00DAPref.prefPane`, a System Preferences pane. Not examined.
- `RDUSB00DASetupd`, a launch daemon. Not examined.

The kernel extension and the CoreMIDI plug-in are both dead on Apple Silicon.
They are x86_64 only, and the kernel extension needs IOAudioFamily.
