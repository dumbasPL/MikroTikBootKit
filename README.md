# MikroTikBootKit

MikroTik RouterOS (x86 (CHR and bare metal), arm64 CHR) BootKit that that patches a stock RouterOS
install on the fly to activate it. No binaries are modified, updates work as expected.

EFI only!

## Download

Grab the stick image for your target from the
[releases page](https://github.com/dumbasPL/MikroTikBootKit/releases):

| file | use |
| --- | --- |
| `bootkit-auto.img` | both loaders, auto detects and boots routeros (recommended, requires a second USB drive) |
| `bootkit.img` | x86 installer stick |
| `bootkit-arm64.img` | arm64 CHR installer stick |

## Use

* **The auto stick** boots the first RouterOS kernel it finds and needs no
  setup at all.

* **Installer sticks** start an install menu: pick the RouterOS EFI partition,
  then either
  * **direct** - the loader is copied to the target EFI partition and a boot entry is
    created; the stick is no longer needed, or
  * **removable** - same as auto, but with explicit RouterOS partition selection
    in case the auto detection doesn't work.

  For advanced use cases, you can also run the loader with `--install` to start the installer from anywhere.

The installed config carries a `debug=` line: set it to `1` for boot
diagnostics on the serial console, `0` for normal use.

## Build

Needs a Linux host with `python3`, `cpio` and `mtools`, plus `clang` + `lld`
for the loaders (a PE-targeting gcc such as `x86_64-w64-mingw32-gcc` works for
x86 too). The arm64 CHR kit additionally needs a host `arm-linux-gnueabihf-gcc`.
Missing toolchains are built automatically on first use.

```sh
make          # everything: x86 + arm64 + the auto stick
make x86      # only x86 (no ARM toolchain needed)
make arm64    # only arm64 CHR
make DEBUG=1  # test build: serial console + verbose logs by default
make clean
```

## Test

`./vmtest.sh` runs the whole chain under QEMU against a copy of a RouterOS
image (`IMG_SRC=...`, or an installer ISO; `ARCH=arm64` for the arm CHR):

```sh
./vmtest.sh test            # direct install, boot and check
./vmtest.sh test-removable  # removable install
./vmtest.sh test-auto       # auto stick, nothing installed
./vmtest.sh cmd "/system license print"
./vmtest.sh stop
```

See `AGENTS.md` for requirements, options and details.  
See `docs/ptrace-init-preload.md` for various reverse engineering notes

## AI disclaimer

Human architected, LLM written, human reviewed and tested.

## License

[WTFPL](LICENSE) - Do What The Fuck You Want To Public License, Version 2.

Based on the ideas from [MikroTikPatch-OSS](https://github.com/dumbasPL/MikroTikPatch-OSS)