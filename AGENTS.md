# MikroTikBootKit

Boot-time tooling for MikroTik RouterOS images.  Two pieces:

* **`bootloader/`** — the EFI side: a small self-contained EFI bootloader, installed as
  `\EFI\BOOT\BOOTKIT.EFI` and reached through a `Boot####` entry.  It replaces
  the EFI shell + `startup.nsh` trick the kit used before: it reads the stock
  kernel from `\EFI\BOOT\BOOTX64.EFI` (left in place there, so a RouterOS
  update overwrites it with the new kernel and the loader picks that up) and
  carries the initramfs (a cpio with `ptrace_init`, built and embedded by the
  `Makefile`) inside the loader image, then enters the kernel through the x86
  EFI handover protocol with a `struct boot_params` it fills in.  The entry
  point is the kernel's own EFI stub, so the EFI runtime environment the init
  expects is preserved.  Built with clang (`--target=x86_64-unknown-windows`,
  PE32+, subsystem 10), freestanding, no gnu-efi and no libc.

  The loader is config driven: it reads `\BOOTKIT.CFG` from the root of the
  partition it was booted from.  The config names the ESP where RouterOS is
  installed by its HD() device path identity (partition signature, start and
  size - not the bus topology, so the same partition is found when the disk is
  plugged into another port).  The loader locates that partition, reads
  `\EFI\BOOT\BOOTX64.EFI` from it and boots it; a RouterOS update overwriting
  that file is picked up on the next boot.

  `target=auto` in the config is *auto mode*: the loader installs nothing and
  boots the first RouterOS kernel it can find instead of following a
  partition identity.  `bootkit-auto.img` is the stick built around it: it
  carries **both** loaders (`\EFI\BOOT\BOOTX64.EFI` and
  `\EFI\BOOT\BOOTAA64.EFI`) and `\BOOTKIT.CFG` = `target=auto`
  (`bootloader/auto.cfg`), so the same stick boots on x86 and on the arm64
  CHR - whichever loader the firmware starts scans every file system the
  firmware exposes and boots the first file that looks like the stock
  RouterOS kernel: on x86 a bzImage setup header with the 64-bit EFI handover
  entry, on arm64 the XZ stock-initramfs stream the loader needs for the
  initrd.  The loader's own file (the stick has it at that very path) matches
  neither check, so the stick never boots itself.  Nothing is written
  anywhere - no config, no boot entry - so the target stays untouched and the
  stick works as a rescue/utility medium; with no kernel found (or with
  `--install`) the loader falls back to the install menu.

  If the config is missing, unreadable or points at a partition that is not
  there, the loader runs its install menu instead: pick the RouterOS ESP, then
  pick a mode -

  * **direct** - copy the loader to the target as `\EFI\BOOT\BOOTKIT.EFI`,
    write `\BOOTKIT.CFG` there and create the `Boot####` entry (first in
    `BootOrder`);
  * **removable** - keep the loader where it is (e.g. a USB stick) and write
    only `\BOOTKIT.CFG` next to it, pointing at the target; nothing is copied
    and no boot entry is created.

  Both end with "press any key to reboot".  `--install` in the load options
  forces the menu even when a valid config exists.

  The **arm64 CHR** works the same way through a second, AArch64 loader
  (`bootkit-arm64.efi`, installed stick as `\EFI\BOOT\BOOTAA64.EFI` in
  `bootkit-arm64.img`).  It boots the stock kernel at
  `\EFI\BOOT\BOOTAA64.EFI` with LoadImage/StartImage - the arm64 kernel is a
  PE/COFF EFI application and its stub keeps the EFI runtime environment -
  and passes the initramfs through the kernel's `initrd=<file>` command line
  option, which the loader serves from a one-file RAM volume.  That is needed
  because the 5.6 arm64 stub has no LoadFile2 initrd support and the boot is
  ACPI (no device tree to put `linux,initrd-start` in).  The stock CHR
  initramfs (the stage-1 `/init`) is XZ-compressed inside the kernel image
  and a kernel-supplied initrd replaces it, so the loader locates that XZ
  stream and hands the kernel "[stock stream][the kit's cpio]" as one initrd
  - Linux unpacks concatenated archives, so the stock rootfs and the kit's
  `/ptrace_init` both appear.  The arm64 CHR's userspace is **AArch32**
  (armv7, soft-float), so the probe and the tracer are arm32 binaries; see
  `bootkit/` below.

  `make DEBUG=1` makes this a *test build*: new configs get
  `debug=1`, which adds the serial console to the x86 kernel command line
  (`console=ttyS0,115200n8 bootkit_debug=1`; on arm64 the base line already
  carries `console=ttyAMA0,115200n8`, so only `bootkit_debug=1` is added) and
  turns on the verbose tracer/probe logs.  The default (production) build
  writes `debug=0`: only the important probe lines (licence state, the three
  patches) plus errors.  The
  `debug=` line in `\BOOTKIT.CFG` overrides this per boot; edit it to switch
  either way without rebuilding.

* **`bootkit/`** — an alternative initramfs init (`rdinit=/ptrace_init`)
  (`bootkit/ptrace_init.c`) that ptrace-attaches to the real init and waits
  for it to mount the tmpfs on `/ram`.  As soon as the mount succeeds the
  tracer writes an embedded `LD_PRELOAD` probe to `/ram/ldpreload.so`,
  detaches the init and exits.
  The probe is put into the environment before `/init` is exec'd, so every
  dynamically linked binary started after the mount (`sysinit`, `mode`,
  `loader`, all services) loads it and its constructor logs the binary's name
  to `/dev/console`.  On top of the logging the probe carries the embedded
  keygen (`keygen.c`): in `/nova/bin/mode` it
  signs and installs the licence with the custom key pair, and in `mode` and
  `keyman` it replaces the licence public key the stock verifier builds on its
  stack.  Nothing in the RouterOS image is modified on disk.
  The arm64 CHR runs an AArch32 (armv7 soft-float) userspace, so for it the
  tracer is built as an arm32 static musl binary and the probe as an armv7
  soft-float shared object (`.toolchain/arm-musl`, `tools/musl_arm.sh`); the
  licence key is materialised differently there (a `.text` literal pool plus
  an arithmetic word, see `docs/ptrace-init-preload.md`).

Design notes, the target behaviours it relies on and the observed boot log:
`docs/ptrace-init-preload.md`.

## Layout

```
bootkit/                    the RouterOS-side initramfs kit (i386 and arm32)
  ptrace_init.c             alternative init that drops the probe (static)
  preload.c                 LD_PRELOAD probe: console log, in-memory key patch,
                            embedded keygen glue
  keygen.c                  embeddable licence keygen (Curve25519 EC-KCDSA)
bootloader/                 the EFI-side loaders (-> \EFI\BOOT\BOOTKIT.EFI)
  efi_main.c                entry point: config -> boot, otherwise install menu
  efi.[ch]                  minimal EFI subset: types, console, files, paths
  bootabi.h                 x86 boot protocol structs (setup header, boot_params)
  boot.c                    x86_64: loads \EFI\BOOT\BOOTX64.EFI and
                            EFI-handovers into it
  boot_arm64.c              AArch64: LoadImage/StartImage's
                            \EFI\BOOT\BOOTAA64.EFI and passes "[stock XZ
                            initramfs][kit cpio]" through the stub's initrd=
  initrdvol.[ch]            the one-file RAM volume that initrd= reads from
  config.[ch]               \BOOTKIT.CFG: target ESP identity (or the "auto"
                            magic value), read/write/find
  vars.c                    Boot#### creation and BootOrder update
  installer.c               install menu (direct / removable)
  auto.cfg                  the \BOOTKIT.CFG of bootkit-auto.img (target=auto)
tools/musl_i386.sh          builds the local i486-musl toolchain (downloads
                            and compiles musl; picked up by the Makefile)
tools/musl_arm.sh           builds the armv7-musl toolchain for the arm64 CHR
                            (needs a host arm-linux-gnueabihf-gcc)
tools/embed.py              writes a binary as a C array header (the probe into
                            ptrace_init, the cpio into the EFI loaders)
tools/mkinitrd.sh           packs ptrace_init as the cpio/newc initramfs
tools/bootkit_img.sh        builds a bootkit*.img USB stick (MBR + FAT ESP)
                            from <src> <dest> file pairs
tools/mbr.py                writes the stick MBR (partition + fixed signature)
Makefile                    builds both kits, both loaders and the three sticks
vmtest.sh                   prepare/install/boot/interact with a test image copy,
                            install a clean image from an installer ISO (x86) or
                            unpack a CHR *.img.zip (arm64)
vmconsole.py                serial-console helper used by vmtest.sh cmd and the
                            bootkit/RouterOS installer drivers
.github/workflows/test.yml  whole-chain CI: iso-install + direct/removable/auto
                            tests on CHR and x86, plus arm64 (TCG on hosted
                            runners)
.github/workflows/daily.yml daily call of test.yml on the latest RouterOS
docs/ptrace-init-preload.md design + findings + verified log
```

## Build

```sh
make                        # production build of both architectures:
                            #   ./ptrace_init      static i386, ~90 KB with the probe
                            #   ./bootkit.efi      x86_64 EFI app, ~110 KB with the
                            #                      embedded initramfs
                            #   ./bootkit.img      32 MB x86 USB stick image
                            #   ./ptrace_init-arm  static arm32 (armv7) for the CHR arm64
                            #   ./bootkit-arm64.efi  AArch64 EFI app
                            #   ./bootkit-arm64.img  32 MB arm64 USB stick image
                            #   ./bootkit-auto.img   32 MB both-arch auto stick
                            #                      (both loaders + target=auto)
make x86                    # only the x86_64 pair (no arm toolchain needed)
make arm64                  # only the arm64 pair (needs clang; the arm musl
                            # toolchain is built automatically)
make bootkit-auto.img       # only the auto stick (needs both, as make does)
make DEBUG=1                # test build: serial console + verbose logs
make clean                  # remove the generated artifacts and .build/
```

`make` skips arm64 and `bootkit-auto.img` with a warning when clang +
lld-link or the ARM cross compiler are not installed; `make arm64` and
`make bootkit-auto.img` fail in that case.  The other build
variables are `EFI_CC=` (x86 loader compiler), `ARM_CC=`, `USB_MB=` and `OUT=`.

`bootkit.img` / `bootkit-arm64.img` are 32 MB MBR disks with one FAT EFI
system partition (type 0xEF, labels `BKINSTALL` / `BKINSTALL64`) holding
`\EFI\BOOT\BOOTX64.EFI` = `bootkit.efi` and `\EFI\BOOT\BOOTAA64.EFI` =
`bootkit-arm64.efi` respectively.  `bootkit-auto.img` (label `BKAUTO`) has the
same layout with *both* loaders plus `\BOOTKIT.CFG` = `target=auto`.  Flash
the one for the target and boot the router from it (the firmware's
removable-media fallback finds it):

```sh
dd if=bootkit.img of=/dev/sdX bs=4M conv=fsync status=progress         # x86
dd if=bootkit-arm64.img of=/dev/sdX bs=4M conv=fsync status=progress   # arm64 CHR
dd if=bootkit-auto.img of=/dev/sdX bs=4M conv=fsync status=progress    # both, auto
```

`USB_MB=<n>` changes the image size.  With no `\BOOTKIT.CFG` on the stick the
loader starts its direct/removable install menu; a removable install writes
the config to the same FAT partition, after which the stick can boot RouterOS
without touching the router's NVRAM.  The auto stick ships that config as
`target=auto`, so it boots the first RouterOS kernel it finds and writes
nothing at all.

The EFI loader is built by the same Makefile; it defines the small EFI subset
it needs itself (`-nostdlib`, no gnu-efi), so apart from the usual build tools
(`python3`, `cpio`, `mtools`) only an EFI-capable compiler is needed: clang +
lld-link (preferred), or a gcc that targets PE such as
`x86_64-w64-mingw32-gcc` (`make EFI_CC=x86_64-w64-mingw32-gcc`; the Makefile
picks it automatically when clang is missing).  Native Linux gcc cannot
produce the relocatable PE by itself - that needs a PE linker or gnu-efi's
self-relocator.
The build packs the freshly built `ptrace_init` into a cpio (`initrd.cpio`,
embedded via the generated `bootloader/initrd_so.h`), so `bootkit.efi` is
self-contained: only the stock kernel sits next to it on the ESP, plus the
`Boot####` entry that points at the loader.  The kernel path and the base/debug
command line fragments are at the top of `bootloader/boot.c` (x86_64) and
`bootloader/boot_arm64.c` (arm64; the base line also sets `console=ttyAMA0`,
the QEMU virt PL011, because the CHR console lives there); the `debug=`
setting in `\BOOTKIT.CFG` picks which fragments are used on each boot.

The EFI loaders define the small EFI subset they need themselves
(`-nostdlib`, no gnu-efi).  The AArch64 one must be built with clang +
lld-link (`--target=aarch64-unknown-windows`; aarch64 UEFI uses the standard
AAPCS64, so `EFIAPI` is empty there, unlike the x86 ms_abi).

The LD_PRELOAD probe (bootkit/preload.c) is ordinary C linked without libc
(-nostdlib): no DT_NEEDED entry, the imports are bound at load time by the
dynamic linker against the libc already in the process (RouterOS /lib/libc.so).

The licence tooling is built in: `keygen.c` is the standalone RouterOS keygen
trimmed to the generation role (no CLI, no mode2 hand-over, no `exit()`,
clean `kg_generate()`/`kg_error()` API) and is `#include`d by `preload.c`.
The key material is hard-coded in `keygen.c` / `preload.c`; the environment
variables `CUSTOM_LICENSE_PUBLIC_KEY` / `CUSTOM_LICENSE_PRIVATE_KEY` (the
pair) and `MIKRO_LICENSE_PUBLIC_KEY` (the stock key) override it at build
time.

The init binaries are always built against musl: `make` uses the local
`.toolchain/i386-musl` and `.toolchain/arm-musl`, and if one is missing or its
`tools/musl_*.sh` changed it runs that script again (the i386 one needs a
multilib host gcc -m32, the arm one a host `arm-linux-gnueabihf-gcc`; both
fetch musl if it is not cached).  They are static, so they run in the
initramfs with no libraries.  The arm32 probe is built soft-float with the
same sysroot (`-mfloat-abi=soft -nostdlib`) so it matches the AArch32
RouterOS processes it is preloaded into.

## Test

Needs: KVM (the default accelerator on x86_64; use `ACCEL=tcg` where there is
no `/dev/kvm`.  The hosted CI runners expose `/dev/kvm` to root only, so
`test.yml` chmods it for the runner user before probing), `clang` + `lld` (or
`x86_64-w64-mingw32-gcc` for the x86 loader only), OVMF (Arch's `edk2-ovmf` or
Debian/Ubuntu's `ovmf` - the paths are found automatically and
`OVMF_CODE`/`OVMF_VARS` override), `mtools` (`mformat`/`mmd`/`mcopy` also
build the sticks), `cpio`, and a RouterOS x86 image.  Use a *copy* of the
image; the ESP (partition 1) is rewritten.  Always kill the VM by pidfile
when done — do not leave QEMU instances running.  Installing from an ISO
additionally needs `isoinfo` (genisoimage) or `xorriso` and a `mikrotik-*.iso`.

For the **arm64 CHR** set `ARCH=arm64` and point `IMG_SRC` at the stock
`chr-<ver>-arm64.img.zip` (or an extracted `.img`; `vmtest.sh` unpacks a zip
itself).  That needs `qemu-system-aarch64` (Ubuntu: `qemu-system-arm`), the
matched 64 MiB AAVMF pair (Ubuntu: `qemu-efi-aarch64`; Arch: `edk2-aarch64`;
`AAVMF_CODE`/`AAVMF_VARS` override), `ipxe-qemu` on Debian/Ubuntu (the `virt`
machine's default virtio NIC loads `efi-virtio.rom` from it) and a host
`arm-linux-gnueabihf-gcc` for the arm32 probe.  The arm64 run always uses TCG
with `-cpu cortex-a72` (the CHR arm64 kernel hangs with `-cpu max`) and boots
to `CHR Login:` on `ttyAMA0`.  Note that `/system check-installation` fails on
arm64 CHR under QEMU by itself (the empty-DTB capability-file check),
independent of the kit.

`./vmtest.sh test` does all of the following on a copy (direct install).
`./vmtest.sh prepare` builds the kit, the test image and copies the arch's
boot image (`bootkit.img` for x86_64, `bootkit-arm64.img` for arm64) as the
installer stick; `./vmtest.sh install` boots stick + target, drives the
installer over the serial console (`INSTALL_MODE=1` direct or `=2` removable)
and restores the target kernel; `./vmtest.sh boot` then starts the target
normally (through the entry the installer created) and `./vmtest.sh
test-removable` / `boot-removable` cover the removable mode (stick first,
bootindex); `./vmtest.sh test-auto` / `boot-auto` build `bootkit-auto.img`
(so the arm64 side is needed as well) and boot it with the target: nothing is
installed, the loader scans the partitions and boots the target's kernel by
itself; `./vmtest.sh cmd "<cli command>"` logs into the running VM as
admin/admin.  The harness builds a test build (`DEBUG=1`, override with
`DEBUG=0`); with a production image `check` shows no tracer/load lines and the
tracer/probe output only appears after flipping `debug=1` in the config:

```sh
# 1. build (ptrace_init, the initramfs and the loader with it embedded)
make

# 2. test image: copy, make it CHR mode (no licence needed); nothing is added
#    to its ESP - the installer does that
IMG=./.work/pb-test.img         # clone of the clean x86 image (on real disk)
cp ../x86-7.23.7-clean.img $IMG
printf '\001' | dd of=$IMG bs=1 seek=$((0x150)) conv=notrunc status=none   # MBR mode flag -> CHR

# 3. USB boot image: make already made bootkit.img (32 MB MBR disk with
#    a FAT ESP holding \EFI\BOOT\BOOTX64.EFI).  ./vmtest.sh prepare copies it
#    to the scratch dir and uses it as the installer stick.
cp bootkit.img /tmp/stick.img

# 4. run the installer: boot stick + target, pick the RouterOS ESP, pick the
#    mode (1 direct / 2 removable) and reboot.  ./vmtest.sh install automates
#    this (the target kernel is moved aside for the run so the stick boots).
qemu-system-x86_64 ... -drive ...file=/tmp/stick.img -device usb-storage,...d2... \
  ... -drive if=pflash,format=raw,file=/tmp/vars.fd ...
python3 vmconsole.py /tmp/serial.sock --install 1

# 5. after a direct install boot the target only; the created entry starts
#    \EFI\BOOT\BOOTKIT.EFI, which reads \BOOTKIT.CFG, finds the RouterOS ESP
#    and boots \EFI\BOOT\BOOTX64.EFI with the embedded initramfs.  With a
#    removable install boot stick + target (vmtest.sh boot-removable).
grep -a BdsDxe /tmp/serial.log           # Boot000N "MikroTikBootKit" -> BOOTKIT.EFI
grep -a efiboot /tmp/serial.log          # loader messages
grep -a ptrace-init /tmp/serial.log      # tracer messages
grep -a ldpreload /tmp/serial.log        # every binary that loaded the probe
tail -c 100 /tmp/serial.log              # should end with "CHR Login:"
kill $(cat /tmp/qemu.pid)
```

### Installing a clean image from an ISO

`./vmtest.sh iso-install [ISO] [OUT]` produces a fresh "clean image" from an
installer ISO without touching the old one: it boots the ISO under OVMF (EFI
mode, which is what makes the stock installer create the FAT EFI partition the
tests expect; a copy of the ISO gets `console=ttyS0,115200n8` added to its
refind options so the installer talks to the serial socket), answers the
installer menu (default package selection) and writes the 128 MB image to
`$WORK/clean.img` (or `$ISO_OUT`; `IMG_SIZE=` changes the size; the default
ISO is the newest `mikrotik-*.iso` next to the script).  It then boots the new
image once to run its first login (answers the licence question with "y" and
quits the agreement pager with q + Enter, then sets the admin password to
admin/admin), so the result is usable like the older clean images and `cmd`
works right away.  The layout is the same the older clean images have
(partition 1 at LBA 2048, the stock kernel at `\EFI\BOOT\BOOTX64.EFI`).

`prepare` also takes an ISO directly and turns it into `$WORK/clean.img` if
that does not exist yet, so a full run from an ISO is:

```sh
IMG_SRC=./mikrotik-7.24.4.iso MODE=chr ./vmtest.sh test
```

For the arm64 CHR the whole run is (a stock image, no ISO):

```sh
ARCH=arm64 IMG_SRC=./chr-7.24.4-arm64.img.zip ./vmtest.sh test
ARCH=arm64 IMG_SRC=./chr-7.24.4-arm64.img.zip ./vmtest.sh test-removable
# or with an extracted image: ARCH=arm64 IMG_SRC=./chr-7.24.4-arm64.img ...
```

The harness walks the first login (licence question/pager and the forced
password change to admin/admin) on its own, so `./vmtest.sh cmd "/system
license print"` works on a stock image too.

`.github/workflows/test.yml` runs all of that on every push/PR in a CHR and an
x86 job, plus an arm64 job on the stock CHR `*.img.zip`: it caches the
i386/arm musl toolchains and the installer ISO / CHR image, installs the clean
image once per job and then runs the direct and the removable test with
loader/tracer/probe and licence assertions.
The workflow probes the QEMU accelerator and falls back to `ACCEL=tcg` with a
longer `WAIT` where KVM is not usable; `/dev/kvm` is root-only on the hosted
runners, so the probe step chmods it for the runner user first.  The RouterOS
version is resolved first from `upgrade.mikrotik.com` (`NEWESTa7.<channel>`,
the endpoint the routers use): the newest release on the `ROUTEROS_CHANNEL`
repository variable (default `stable`), or an explicit `version` input when
dispatching.
`.github/workflows/daily.yml` calls the test workflow once a day on that latest
version.

### Installing on real hardware

1. Flash the stick for the target (x86: `bootkit.img`, arm64 CHR:
   `bootkit-arm64.img`) and boot the router from it (the firmware's
   removable-media fallback finds `\EFI\BOOT\BOOTX64.EFI` /
   `\EFI\BOOT\BOOTAA64.EFI`):

   ```sh
   dd if=bootkit.img of=/dev/sdX bs=4M conv=fsync status=progress         # x86
   dd if=bootkit-arm64.img of=/dev/sdX bs=4M conv=fsync status=progress   # arm64
   ```

   (Copying `bootkit.efi` / `bootkit-arm64.efi` to a plain FAT stick as the
   same path works the same way if there is no `.img` at hand.)
2. The loader finds no `\BOOTKIT.CFG` and starts the install menu: pick the
   EFI partition where RouterOS is installed, then the mode.
   * **direct**: it copies itself to `\EFI\BOOT\BOOTKIT.EFI` there, writes
     `\BOOTKIT.CFG` and creates a `MikroTikBootKit` `Boot####` entry first in
     `BootOrder`.  The stick is no longer needed.
   * **removable**: it writes `\BOOTKIT.CFG` to the stick only; nothing on the
     router changes and no NVRAM is touched.  Keep the stick plugged in and
     select it in the firmware boot menu (or give it a boot entry) - the
     loader will follow the config and boot RouterOS from the internal ESP.
3. Both modes end with "press any key to reboot".

The stock kernel at `\EFI\BOOT\BOOTX64.EFI` (x86) / `\EFI\BOOT\BOOTAA64.EFI`
(arm64) is never touched, so a RouterOS update can replace it freely: the
config still finds the partition, and the loader boots the updated kernel with
the kit's initramfs.  If an update also
rewrites `BootOrder` (some installers re-add their own entry), just move
`MikroTikBootKit` back to the top once with `efibootmgr -o ...` or the
firmware menu.  The menu can also be forced with `--install` (e.g. from the
UEFI shell: `fs0:\EFI\BOOT\BOOTKIT.EFI --install`).

For just booting an already installed system with the kit's initramfs -
nothing installed, no boot entry, the router left untouched - flash
`bootkit-auto.img` instead: it carries both loaders and a `\BOOTKIT.CFG` with
`target=auto`, so whichever loader the firmware starts scans the partitions
and boots the first RouterOS kernel it finds (its own files do not match the
kernel checks, so it never boots itself).  Keep the stick plugged in and
select it in the boot menu, or give it a `Boot####` entry; a RouterOS update
does not disturb it.

Expected installer output (direct mode):

```
efiboot: no valid \BOOTKIT.CFG here, starting the installer
efiboot: installer mode
efiboot: debug logging is on (edit debug= in \BOOTKIT.CFG to change)
efiboot: EFI partitions:
efiboot:   1) (no label), 33 MB
efiboot:      PciRoot(0x0)/Pci(0x4,0x0)/USB(0x0,0x0)/HD(1,MBR,...)
efiboot:   2) BKINSTALL, 31 MB, this installer
efiboot:      PciRoot(0x0)/Pci(0x4,0x0)/USB(0x1,0x0)
efiboot: select the EFI partition where RouterOS is installed [1-2] (q to cancel): 1
efiboot: installation mode:
efiboot:   1) direct: copy the loader + config to the target and create the boot entry
efiboot:   2) removable: keep the loader here, write only the config (target stays untouched)
efiboot: select mode [1-2] (q to cancel): 1
efiboot: boot entry Boot0009 -> \EFI\BOOT\BOOTKIT.EFI
efiboot: direct install done, copied 112640 bytes to partition 1
efiboot: press any key to reboot
```

Expected loader output (before the kernel takes over):

```
BdsDxe: loading Boot000N "MikroTikBootKit" ... FilePath(\EFI\BOOT\BOOTKIT.EFI)
BdsDxe: starting Boot000N "MikroTikBootKit" ...
efiboot: MikroTik boot kit loader
efiboot: kernel 5.6.3-64 (gitlab-runner@cicd-a13.mt.lv) #1 SMP ...
efiboot: initrd 94720 bytes, booting (rdinit=/ptrace_init)
efiboot: initrd 94720 bytes, booting (rdinit=/ptrace_init console=ttyS0,115200n8 bootkit_debug=1)
```
(the second line is with `debug=1`; on arm64 the line is
`efiboot: initrd 171120 bytes (stock 81008 + kit 90112), booting
(rdinit=/ptrace_init initrd=initrd.cpio console=ttyAMA0,115200n8
bootkit_debug=1)` - the stock XZ initramfs plus the kit's cpio)

Expected loader output in auto mode (`bootkit-auto.img`, no install; the
firmware starts whichever loader it finds, here the x86 one on a USB stick):

```
efiboot: MikroTik boot kit loader
efiboot: auto mode: looking for the first RouterOS kernel
efiboot: auto: kernel found on PciRoot(0x0)/Pci(0x4,0x0)/USB(0x0,0x0)/HD(1,MBR,...)
efiboot: kernel 5.6.3-64 (gitlab-runner@cicd-a13.mt.lv) #1 SMP ...
efiboot: initrd 94720 bytes, booting (rdinit=/ptrace_init ...)
```

Expected tracer output (with `debug=1`):

```
[ptrace-init] tracing pid 1
[ptrace-init] exec /init (LD_PRELOAD=/ram/ldpreload.so)
[ptrace-init] following the init into /newroot
[ptrace-init] pid 1 mounts tmpfs on /ram
[ptrace-init] bind-mounted /proc/self/fd/N -> /ram/ldpreload.so, detaching
[ldpreload] loaded by /sbin/sysinit (pid=119)
[ldpreload] loaded by /nova/bin/mode (pid=129)
[ldpreload] mode: licence key patched (1 site)
[ldpreload] mode: licence generated (system-id xxxxxxxxxxx)
[ldpreload] loaded by /nova/bin/keyman (pid=135)
[ldpreload] keyman: licence key patched (1 site)
[ldpreload] loaded by /nova/bin/loader (pid=130)
[ldpreload] loader: memcmp GOT patched (1 slot)
... (47 loads in total)
```

On a CHR-mode boot `/system license print` then shows `level: p-unlimited`
with that `system-id`; the blob is written to sector 0 of the disk, so later
boots log `licence already installed` and keep it (the keygen only re-signs
when the stored one no longer verifies).

### Notes / gotchas

* **Known versions:** works on 7.23.7 and 7.24.4 (both x86 and CHR mode,
  40-47 loads, boot reaches the login prompt).  The arm64 CHR of those
  versions is covered too: its userspace is AArch32, so the same probe is
  built as armv7 soft-float and the licence key is patched in its own
  encoding (a `.text` literal pool plus an arithmetic word; see
  `docs/ptrace-init-preload.md`).  On 7.24.4 the kernel refuses
  `PROT_EXEC` mappings of tmpfs files, so the probe is not written to `/ram`
  but bind-mounted there from a copy kept on the initramfs; the same kernel
  also rejects `mprotect(PROT_READ|PROT_WRITE|PROT_EXEC)`, so the in-memory
  key patch makes the text pages `rw` and restores `r-x` afterwards.  Details:
  `docs/ptrace-init-preload.md`.
* **The loader's key is never replaced.**  RouterOS cross-checks the
  loader's embedded key material against other state at boot and changing it
  (text/rodata) aborts the system supervisor (`/nova/bin/sys2`).  Instead the
  probe redirects the loader's `memcmp` GOT slot to an "always equal" stub -
  the trick the recovered reference tool uses - so its licence verifier
  accepts the custom signature.  GOT only; no text or key bytes are changed.
* **The licence works on CHR and x86.**  CHR binds to the VM UUID plus the
  software id; x86 mode (7.23.7 and 7.24.4) binds to the hardware-derived
  software id.  Without the loader hook x86 reports
  `nlevel: 6` / `features: extra-channels` but keeps a rolling ~72 h
  `expires-in`; with the hook the loader also accepts the signature, the
  countdown is gone and the demo counter stays at 0.  Details:
  `docs/ptrace-init-preload.md`.
* **Do not boot with QEMU's `-kernel` + `-initrd`.**  It was tried: the
  wrapper runs, but the stock init then fails (`opendir: No such file or
  directory` → `ERROR: no system package found!`) and the kernel panics.  The
  disk is fine (`/dev/vda{,1,2}` present, `/dev/vda2` mounts), and the same
  image boots under OVMF — the init appears to need the EFI environment.  Use
  the EFI loader above: its handover entry *is* the kernel's EFI stub, so the
  init still gets the EFI runtime environment.
* The kit is added to the image's own ESP *by its installer*: direct mode
  copies the loader to `\EFI\BOOT\BOOTKIT.EFI`, writes `\BOOTKIT.CFG` and
  creates the `Boot####` entry; removable mode only writes `\BOOTKIT.CFG` on
  the medium the loader runs from.  The stock kernel stays at
  `\EFI\BOOT\BOOTX64.EFI` (4,036,832/4,024,544 bytes depending on the version)
  and is what the loader boots; the initramfs is embedded in `bootkit.efi`.
* The config is `target=mbr:<sig>:<start>:<size>` (or `gpt:` with a 32-hex
  partition GUID), taken from the target volume's HD() device path node.  The
  identity deliberately excludes the bus topology, so a disk moved to another
  port still matches; a different disk does not and the menu comes up again.
  The device path text is in a comment line for debugging (and shown in the
  installer listing).  The other accepted value is the literal `auto`
  (`bootloader/auto.cfg` on `bootkit-auto.img`): no identity, no install -
  the loader boots the first RouterOS kernel it finds and the target is left
  alone.  `\BOOTKIT.CFG` is read up to 512 bytes (`CONFIG_MAX`), so keep the
  `target=` line inside that; the shipped auto config has no `debug=` line
  and gets the build's default.
* `debug=0|1` in the config is read on every boot: 1 appends
  `console=ttyS0,115200n8 bootkit_debug=1` to the kernel command line (the
  trace/probe logs become visible on serial) and 1/0 is passed to the
  initramfs via the command line, so ptrace_init and the probe switch between
  verbose and important-only logging.  Trade-off: with the serial console off,
  the kit's messages go to the default console (tty0) and are invisible on
  serial, but RouterOS's own login still appears there (userspace drives the
  port).
* The installer writes the boot entry with the runtime services: it reuses an
  existing `Boot####` for the same file path if there is one, otherwise takes
  the next free number, and prepends it to `BootOrder`.  Variable names in
  OVMF can be longer than 15 characters, so the `GetNextVariableName` buffer
  must not be a small stack array (that cost one debugging round).
* A RouterOS update that replaces `\EFI\BOOT\BOOTX64.EFI` is harmless: the
  loader and its boot entry stay, and the next boot loads the new kernel with
  the kit's initramfs.  (Verified by swapping a 7.23.7 kernel into a 7.24.4
  test image: the loader logged the new build date with no reinstall.)
* `serial=test` on the USB storage is how the earlier live tests booted this
  image; it also feeds the x86 software-id (irrelevant in CHR mode).
* For an interactive check, use a serial *socket* instead of `file:` and log
  in as `admin` (the clean image is `admin`/`admin`), then look at the boot
  console log for the `[ldpreload]` lines; `/ram/ldpreload.so` is on the
  running system's tmpfs.  The first login may walk through a forced password
  change — use a fresh image copy if you need the CLI.
* **arm64 CHR:** the boot is ACPI, so the kernel's EFI stub generates an empty
  DTB and there is no `linux,initrd-start` path for the kit's cpio; the 5.6
  stub has no LoadFile2 initrd support either.  The loader therefore passes
  `initrd=initrd.cpio` and serves the file from a RAM volume it installs on a
  new handle, then points the loaded kernel's `DeviceHandle` at it (the stub
  reads `initrd=` from the kernel image's own device handle).  The stock
  initramfs is an XZ stream inside the kernel's `.data`; since a kernel
  initrd replaces it, the loader locates that stream (magic + footer CRC32)
  and prepends it to the kit's cpio, and Linux unpacks the concatenation.
  Do not point `initrd=` at a file on disk: direct installs would have to
  write it, and removable installs must leave the target alone.
* **arm64 CHR QEMU specifics:** use `-M virt -cpu cortex-a72` (`-cpu max`
  hangs this kernel) and a *matched* 64 MiB AAVMF code/vars pair; the console
  is ttyAMA0, and the arm64 CHR image itself is always CHR mode.  The stock
  `/system check-installation` fails under QEMU on arm64 (RouterOS's ARM
  checker wants `/ram` capability files that the empty DTB cannot provide) -
  that is a pre-existing RouterOS/QEMU limitation, not the kit.
* `docs/ptrace-init-preload.md` lists the target behaviours the logic depends
  on (the init's stage-1 root switch, the early `/ram` mount).
  Re-read it before "simplifying" the tracer loop.
