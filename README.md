# ALC274 / RT1305 speaker EQ

Linux speaker EQ driver for the MECHREVO GM7ZG8M. It applies the machine's
48 kHz EQ profile through the ALC274 amplifier-control bridge.

## Hardware

| Component | ID |
| --- | --- |
| Codec | Realtek ALC274, `10ec:0274` |
| Subsystem | `1d05:1115` |
| Amplifier | RT1305/RT1306 family, `10ec:6251` |
| Amplifier endpoint | `0x10` |

The profile contains 60 filter coefficients and four EQ/HPF controls. The
driver saves the initial EQ state, programs the profile while muted, and
restores the saved state on unload. It reapplies EQ during codec initialization.
Playback at other sample rates uses the saved EQ state.

Tested on Debian kernel `6.12.113+deb13-amd64`: all 64 registers matched the
profile, and left/right speaker playback worked.

## Build

Requires a C compiler, GNU Make, `patch`, matching kernel headers with
`Module.symvers`, and the matching Linux source tree. The patch targets the
Linux 6.12 `sound/pci/hda` source layout.

```sh
make KERNEL_SRC=/path/to/linux-source-6.12
```

The output is `.build/snd-hda-codec-realtek.ko`, a replacement for the Realtek
HDA codec module. Set `KDIR=/path/to/kernel/build` to build for another kernel.
Rebuild and reinstall the module after a kernel upgrade.

## Install

For the running kernel:

```sh
sudo install -D -m 644 .build/snd-hda-codec-realtek.ko \
  /lib/modules/$(uname -r)/updates/alc274-rt1305/snd-hda-codec-realtek.ko
sudo depmod -a
```

If the Realtek codec module is included in your initramfs, rebuild the
initramfs using your distribution's tool. Reboot to load the driver.
The module parameter `rt1305_eq=0` disables the EQ profile.

## Tests

```sh
make test
```

The C tests run the driver logic against a modeled HDA bridge. They cover
EQ programming, error recovery, mute restoration, resume state, polling
limits, hardware matching and sample-rate changes.

## License

[GNU GPL v2](LICENSE) (`GPL-2.0-only`).
