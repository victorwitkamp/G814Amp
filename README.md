# G814Amp

A macOS kernel extension that powers up the two Cirrus Logic CS35L41 speaker amplifiers of the ASUS ROG Strix G814JI, so the
built-in speakers play under macOS.

On this laptop the Realtek ALC294 codec sends audio over I2S to two CS35L41 smart amplifiers, which are controlled over SPI
(the PCH's GSPI3, PCI `8086:7A79`). AppleHDA and AppleALC drive the codec but nothing in macOS configures the amplifiers, so they
stay in reset and the speakers stay silent. G814Amp brings them up and switches them on and off with the audio output.

## What it does

- Matches GSPI3 (`IOPCIPrimaryMatch` `0x7a798086`) and drives its Intel LPSS SSP controller directly (PIO), plus the GPIO pads
  GPP_S0 (the amplifiers' reset, shared) and GPP_D20 (chip select of amplifier 1).
- **Auto mode** (the default, `AutoEnable` in `Info.plist`): when AppleHDA's output engine starts, it resets both amplifiers,
  checks their IDs, applies the OTP trims and errata, configures boost and the audio serial port, and enables them at gain code 4
  (4.5 dB). When the output engine stops, it mutes them and turns them off. Sleep puts them back in reset; wake brings them up
  again. Five failed attempts stop auto mode until you run the steps by hand.
- Logs to the kernel log with the prefix `G814Amp:` (`log show --predicate 'eventMessage CONTAINS "G814Amp"' --last boot`).

## Requirements

- ASUS ROG Strix G814JI (the pins, buses and addresses are this laptop's).
- macOS 15 or later, x86_64. Tested on macOS 15.7.1.
- AppleALC with a layout that routes the speaker pin, e.g. `alcid=22`.

## Install (OpenCore)

Copy `G814Amp.kext` to `EFI/OC/Kexts/` and add it to `Kernel/Add`:

| key | value |
|---|---|
| `BundlePath` | `G814Amp.kext` |
| `ExecutablePath` | `Contents/MacOS/G814Amp` |
| `PlistPath` | `Contents/Info.plist` |
| `Arch` | `x86_64` |
| `Enabled` | `true` |

It does not need Lilu.

## Manual control

Everything is under `sysctl debug.g814amp`:

| sysctl | |
|---|---|
| `cmd` | run a step: `0` read-only survey, `1` probe, `2` configure, `3` enable, `4` status, `5` disable, `6` disable and reset, `9` = 1 + 2 + 3 |
| `gain` | `AMP_GAIN_PCM` code 0-9 used by the next enable (4 = 4.5 dB) |
| `auto` | `1` = follow the output engine; `5` and `6` set it to `0` |
| `state` | bit 0 SSP up, bit 1 probed, bit 2 configured, bit 3 enabled, bit 8 fault |
| `status` | the last verdict line |

For example `sudo sysctl debug.g814amp.cmd=4` prints both amplifiers' PLL lock, IRQ status and power registers to the log.

## Build

```
make
```

builds `build/G814Amp.kext` with clang against the macOS SDK's Kernel.framework (Xcode or the Command Line Tools) and prints the
binary's SHA-256.

## Boot chime

G814Amp works in macOS only. For the OpenCore boot chime on the speakers, the amplifiers have to be enabled in UEFI before
AudioDxe plays: see [G814AmpDxe](https://github.com/victorwitkamp/G814AmpDxe).

## License

The driver is BSD-3-Clause (`LICENSE`). Its CS35L41 power-up, errata and OTP sequences follow Linux's
`sound/soc/codecs/cs35l41-lib.c` and `sound/hda/codecs/side-codecs/cs35l41_hda.c`; `cs35l41_regs.h` (register constants from
Linux's `include/sound/cs35l41.h`) and `cs35l41_otp_maps.h` (OTP trim maps copied from `cs35l41-lib.c`) are GPL-2.0, so a binary
built from this tree carries GPL-2.0 terms unless those parts are replaced.
