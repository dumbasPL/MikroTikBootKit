# MikroTikBootKit

Boot-time tooling for MikroTik RouterOS images.  Two pieces:

* **`bootloader/`** — the EFI side: a small self-contained EFI bootloader, installed as
  `\EFI\BOOT\BOOTKIT.EFI` and reached through a `Boot####` entry.  It replaces
  the EFI shell + `startup.nsh` trick the kit used before: it reads the stock
  kernel from `\EFI\BOOT\BOOTX64.EFI` (left in place there, so a RouterOS
  update overwrites it with the new kernel and the loader picks that up) and
  carries the initramfs (a cpio with `ptrace_init`, built and embedded by
  `build.sh`) inside the loader image, then enters the kernel through the x86
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

  `DEBUG=1 ./build.sh` makes this a *test build*: new configs get
  `debug=1`, which adds `console=ttyS0,115200n8 bootkit_debug=1` to the
  kernel command line and turns on the verbose tracer/probe logs.  The
  default (production) build writes `debug=0`: no console option and only the
  important probe lines (licence state, the three patches) plus errors.  The
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

Design notes, the target behaviours it relies on and the observed boot log:
`docs/ptrace-init-preload.md`.

## Layout

```
bootkit/                    the RouterOS-side initramfs kit
  ptrace_init.c             alternative init that drops the probe (static i386)
  preload.c                 LD_PRELOAD probe: console log, in-memory key patch,
                            embedded keygen glue
  keygen.c                  embeddable licence keygen (Curve25519 EC-KCDSA)
bootloader/                 the EFI-side loader (-> \EFI\BOOT\BOOTKIT.EFI)
  efi_main.c                entry point: config -> boot, otherwise install menu
  efi.[ch]                  minimal EFI subset: types, console, files, paths
  bootabi.h                 x86 boot protocol structs (setup header, boot_params)
  boot.c                    loads \EFI\BOOT\BOOTX64.EFI and EFI-handovers into it
  config.[ch]               \BOOTKIT.CFG: target ESP identity, read/write/find
  vars.c                    Boot#### creation and BootOrder update
  installer.c               install menu (direct / removable)
tools/musl_i386.sh          builds the local i486-musl toolchain (downloads
                            and compiles musl; picked up by build.sh)
build.sh                    builds the bootkit, the bootloader and bootkit.img
vmtest.sh                   prepare/install/boot/interact with a test image copy
vmconsole.py                serial-console helper used by vmtest.sh cmd and the
                            installer driver
docs/ptrace-init-preload.md design + findings + verified log
```

## Build

```sh
./build.sh                  # production build:
                            #   ./ptrace_init (static i386, ~90 KB with the probe)
                            #   ./bootkit.efi (EFI application, ~110 KB with the
                            #   embedded initramfs)
                            #   ./bootkit.img (32 MB USB stick image of the loader)
DEBUG=1 ./build.sh          # test build: serial console + verbose logs
```

`bootkit.img` is a 32 MB MBR disk with one FAT EFI system partition (type
0xEF, label `BKINSTALL`) holding `\EFI\BOOT\BOOTX64.EFI` = `bootkit.efi`.
Flash it to a USB stick and boot the router from it (the firmware's
removable-media fallback finds it):

```sh
dd if=bootkit.img of=/dev/sdX bs=4M conv=fsync status=progress
```

`USB_MB=<n>` changes the image size.  With no `\BOOTKIT.CFG` on the stick the
loader starts its direct/removable install menu; a removable install writes
the config to the same FAT partition, after which the stick can boot RouterOS
without touching the router's NVRAM.

The EFI loader is built by the same script; it defines the small EFI subset it
needs itself (`-nostdlib`, no gnu-efi), so apart from the usual build tools
(`python3`, `cpio`, `mtools`) only an EFI-capable compiler is needed: clang +
lld-link (preferred), or a gcc that targets PE such as
`x86_64-w64-mingw32-gcc` (`EFI_CC=x86_64-w64-mingw32-gcc ./build.sh`; the
script picks it automatically when clang is missing).  Native Linux gcc cannot
produce the relocatable PE by itself - that needs a PE linker or gnu-efi's
self-relocator.
The script packs the freshly built `ptrace_init` into a cpio (`initrd.cpio`,
embedded via the generated `bootloader/initrd_so.h`), so `bootkit.efi` is
self-contained: only the stock kernel sits next to it on the ESP, plus the
`Boot####` entry that points at the loader.  The kernel path and the base/debug
command line fragments are at the top of `bootloader/boot.c`; the `debug=`
setting in `\BOOTKIT.CFG` picks which fragments are used on each boot.

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

The i386 binary is always built against musl: `build.sh` uses the local
`.toolchain/i386-musl`, and if that is missing it runs `tools/musl_i386.sh`
itself (which needs a multilib host gcc -m32 and network access to fetch
musl).  The binary is static, so it runs in the initramfs with no libraries.

## Test

Needs: KVM (`-enable-kvm` is mandatory), `clang` + `lld` (or
`x86_64-w64-mingw32-gcc`), `edk2-ovmf`,
`mtools` (`mformat`/`mmd`/`mcopy` also build `bootkit.img`), `cpio`, and a
RouterOS x86 image.  Use a *copy* of the image; the ESP (partition 1) is
rewritten.  Always kill the VM by pidfile when done — do not leave QEMU
instances running.

`./vmtest.sh test` does all of the following on a copy (direct install).
`./vmtest.sh prepare` builds the kit, the test image and copies `bootkit.img`
as the installer stick; `./vmtest.sh install` boots stick + target, drives the
installer over the serial console (`INSTALL_MODE=1` direct or `=2` removable)
and restores the target kernel; `./vmtest.sh boot` then starts the target
normally (through the entry the installer created) and `./vmtest.sh
test-removable` / `boot-removable` cover the removable mode (stick first,
bootindex); `./vmtest.sh cmd "<cli command>"` logs into the running VM as
admin/admin.  The harness builds a test build (`DEBUG=1`, override with
`DEBUG=0`); with a production image `check` shows no tracer/load lines and the
tracer/probe output only appears after flipping `debug=1` in the config:

```sh
# 1. build (ptrace_init, the initramfs and the loader with it embedded)
./build.sh

# 2. test image: copy, make it CHR mode (no licence needed); nothing is added
#    to its ESP - the installer does that
IMG=../.work/pb-test.img        # clone of the clean x86 image; keep it on real disk, not tmpfs
cp ../x86-7.23.7-clean.img $IMG
printf '\001' | dd of=$IMG bs=1 seek=$((0x150)) conv=notrunc status=none   # MBR mode flag -> CHR

# 3. USB boot image: ./build.sh already made bootkit.img (32 MB MBR disk with
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

### Installing on real hardware

1. Flash `bootkit.img` to a USB stick and boot the router from it (the
   firmware's removable-media fallback finds `\EFI\BOOT\BOOTX64.EFI`):

   ```sh
   dd if=bootkit.img of=/dev/sdX bs=4M conv=fsync status=progress
   ```

   (Copying `bootkit.efi` to a plain FAT stick as `\EFI\BOOT\BOOTX64.EFI` works
   the same way if there is no `bootkit.img` at hand.)
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

The stock kernel at `\EFI\BOOT\BOOTX64.EFI` is never touched, so a RouterOS
update can replace it freely: the config still finds the partition, and the
loader boots the updated kernel with the kit's initramfs.  If an update also
rewrites `BootOrder` (some installers re-add their own entry), just move
`MikroTikBootKit` back to the top once with `efibootmgr -o ...` or the
firmware menu.  The menu can also be forced with `--install` (e.g. from the
UEFI shell: `fs0:\EFI\BOOT\BOOTKIT.EFI --install`).

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
(the second line is with `debug=1`)

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
  40-47 loads, boot reaches the login prompt).  On 7.24.4 the kernel refuses
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
  installer listing).
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
* `docs/ptrace-init-preload.md` lists the target behaviours the logic depends
  on (the init's stage-1 root switch, the early `/ram` mount).
  Re-read it before "simplifying" the tracer loop.
