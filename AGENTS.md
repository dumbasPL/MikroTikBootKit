# MikroTikBootKit

Boot-time tooling for MikroTik RouterOS images.  Two pieces:

* **`efiboot.c`** — a small self-contained EFI bootloader, installed as
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

  It is also its own installer: when it is booted as `\EFI\BOOT\BOOTX64.EFI`
  (the removable-media fallback, i.e. from a USB stick) or with `--install` in
  the load options, it lists the EFI partitions, asks which one to install to,
  copies itself there as `\EFI\BOOT\BOOTKIT.EFI`, creates the `Boot####` entry
  (first in `BootOrder`) and reboots on a key press.  That is how the kit is
  deployed: nothing else has to write NVRAM.

* **`ptrace_init.c`** — an alternative initramfs init (`rdinit=/ptrace_init`)
  that ptrace-attaches to the real init and waits for it to mount the tmpfs on
  `/ram`.  As soon as the mount succeeds the tracer writes an embedded
  `LD_PRELOAD` probe to `/ram/ldpreload.so`, detaches the init and exits.
  The probe is put into the environment before `/init` is exec'd, so every
  dynamically linked binary started after the mount (`sysinit`, `mode`,
  `loader`, all services) loads it and its constructor logs the binary's name
  to `/dev/console`.  On top of the logging the probe carries the embedded
  keygen (`keygen.c`, adapted from MikroTikPatch's): in `/nova/bin/mode` it
  signs and installs the licence with the custom key pair, and in `mode` and
  `keyman` it replaces the licence public key the stock verifier builds on its
  stack.  Nothing in the RouterOS image is modified on disk.

Design notes, the target behaviours it relies on and the observed boot log:
`docs/ptrace-init-preload.md`.

## Layout

```
efiboot.c                   EFI loader + installer (-> \EFI\BOOT\BOOTKIT.EFI):
                            reads the stock kernel from \EFI\BOOT\BOOTX64.EFI,
                            copies the embedded initramfs, fills boot_params
                            and EFI-handover boots the kernel; as \BOOTX64.EFI
                            or with --install it installs itself to an ESP
                            and creates the Boot#### entry
ptrace_init.c               the tool (static i386)
preload.c                   LD_PRELOAD probe: console log, in-memory key patch,
                            embedded keygen glue
keygen.c                    embeddable licence keygen (adapted from MikroTikPatch)
build.sh                    builds + embeds the probe, then the tool and the loader
vmtest.sh                   prepare/install/boot/interact with a test image copy
vmconsole.py                serial-console helper used by vmtest.sh cmd and the
                            installer driver
docs/ptrace-init-preload.md design + findings + verified log
```

## Build

```sh
./build.sh                  # -> ./ptrace_init (static i386, ~90 KB with the probe)
                            #    ./bootkit.efi (EFI application, ~100 KB with the
                            #    embedded initramfs)
```

The EFI loader is built by the same script with clang/lld-link; it defines the
small EFI subset it needs itself (`-nostdlib`, no gnu-efi), so apart from the
usual build tools (`python3`, `cpio`) only clang and lld are needed.  The
script packs the freshly built `ptrace_init` into a cpio (`initrd.cpio`,
embedded via the generated `initrd_so.h`), so `bootkit.efi` is self-contained:
only the stock kernel sits next to it on the ESP, plus the `Boot####` entry
that points at the loader.  The kernel path and command line are compile-time
constants at the top of `efiboot.c`.

The LD_PRELOAD probe (preload.c) is ordinary C linked without libc
(-nostdlib): no DT_NEEDED entry, the imports are bound at load time by the
dynamic linker against the libc already in the process (RouterOS /lib/libc.so).

The licence tooling is built in: `keygen.c` is a copy of MikroTikPatch's
keygen trimmed to the generation role (no CLI, no mode2 hand-over, no
`exit()`, clean `kg_generate()`/`kg_error()` API) and is `#include`d by
`preload.c`.  The key material is baked in at build time from `keys.env`
(`$ROOT/keys.env` when present, otherwise `MikroTikPatch/keys.env`):
`CUSTOM_LICENSE_PUBLIC_KEY` / `CUSTOM_LICENSE_PRIVATE_KEY`, stock key
`MIKRO_LICENSE_PUBLIC_KEY`; both are overridable via environment / `KEYS_ENV`,
and `/keys.env` is gitignored.

It uses, in order: a local `.toolchain/i386-musl`, MikroTikPatch's
`.toolchain/i386-musl` (create it with `MikroTikPatch/tools/musl_i386.sh`), or
a multilib host `gcc -m32`.  The binary is static, so it runs in the
initramfs with no libraries.

## Test

Needs: KVM (`-enable-kvm` is mandatory), `clang` + `lld`, `edk2-ovmf`,
`mtools` (with `mformat` for the installer stick), `cpio`, and a RouterOS x86
image.  Use a *copy* of the image; the ESP (partition 1) is rewritten.  Always
kill the VM by pidfile when done — do not leave QEMU instances running.

`./vmtest.sh test` does all of the following on a copy.  `./vmtest.sh prepare`
builds the kit, the test image and a 2 MB installer stick; `./vmtest.sh
install` boots stick + target, drives the installer over the serial console
and restores the target kernel; `./vmtest.sh boot` then starts the target
normally (through the entry the installer created); `./vmtest.sh cmd "<cli
command>"` logs into the running VM as admin/admin:

```sh
# 1. build (ptrace_init, the initramfs and the loader with it embedded)
./build.sh

# 2. test image: copy, make it CHR mode (no licence needed); nothing is added
#    to its ESP - the installer does that
IMG=../.work/pb-test.img        # clone of the clean x86 image; keep it on real disk, not tmpfs
cp ../x86-7.23.7-clean.img $IMG
printf '\001' | dd of=$IMG bs=1 seek=$((0x150)) conv=notrunc status=none   # MBR mode flag -> CHR

# 3. installer stick: a tiny FAT image with bootkit.efi as \EFI\BOOT\BOOTX64.EFI
#    (the removable-media fallback path; the loader sees its own name and
#    enters installer mode)
dd if=/dev/zero of=/tmp/stick.img bs=1M count=2 status=none
mformat -i /tmp/stick.img -v BKINSTALL ::
mmd -i /tmp/stick.img ::/EFI ::/EFI/BOOT
mcopy -i /tmp/stick.img bootkit.efi ::/EFI/BOOT/BOOTX64.EFI

# 4. run the installer: boot stick + target, pick the target from the list,
#    the installer copies itself to \EFI\BOOT\BOOTKIT.EFI and creates the
#    Boot#### entry, then reboots.  ./vmtest.sh install automates this (the
#    target kernel is moved aside for the run so the stick boots).
qemu-system-x86_64 ... -drive ...file=/tmp/stick.img -device usb-storage,...d2... \
  ... -drive if=pflash,format=raw,file=/tmp/vars.fd ...
python3 vmconsole.py /tmp/serial.sock --install

# 5. boot the target only; the created entry starts \EFI\BOOT\BOOTKIT.EFI,
#    which reads \EFI\BOOT\BOOTX64.EFI and the initramfs embedded in itself
grep -a BdsDxe /tmp/serial.log           # Boot000N "MikroTikBootKit" -> BOOTKIT.EFI
grep -a efiboot /tmp/serial.log          # loader messages
grep -a ptrace-init /tmp/serial.log      # tracer messages
grep -a ldpreload /tmp/serial.log        # every binary that loaded the probe
tail -c 100 /tmp/serial.log              # should end with "CHR Login:"
kill $(cat /tmp/qemu.pid)
```

### Installing on real hardware

1. Put `bootkit.efi` on a USB stick as `\EFI\BOOT\BOOTX64.EFI` (a plain FAT
   stick; no boot entry needed, the firmware's removable-media fallback finds
   it), boot the router from it.
2. The loader lists the EFI partitions it can see and asks where to install.
   Pick the router's ESP; it copies itself there as `\EFI\BOOT\BOOTKIT.EFI`,
   creates a `MikroTikBootKit` boot entry and puts it first in `BootOrder`,
   then reboots on a key press.

The stock kernel at `\EFI\BOOT\BOOTX64.EFI` is never touched, so a RouterOS
update can replace it freely: the loader and its boot entry stay in place and
the next boot loads the updated kernel with the kit's initramfs.  If an update
also rewrites `BootOrder` (some installers re-add their own entry), just move
`MikroTikBootKit` back to the top once with `efibootmgr -o ...` or the
firmware menu.  The installer can also be started with `--install` in the load
options instead of the `BOOTX64.EFI` name (e.g. from the UEFI shell:
`fs0:\EFI\BOOT\BOOTKIT.EFI --install`).

Expected installer output:

```
efiboot: installer mode
efiboot: EFI partitions:
efiboot:   1) (no label), 32 MB
efiboot:      PciRoot(0x0)/Pci(0x4,0x0)/USB(0x0,0x0)/HD(1,MBR,...)
efiboot:   2) BKINSTALL, 1 MB, this installer
efiboot:      PciRoot(0x0)/Pci(0x4,0x0)/USB(0x1,0x0)
efiboot: select the target EFI partition [1-2] (q to cancel): 1
efiboot: installing to partition 1
efiboot: copied 99328 bytes
efiboot: boot entry Boot0009 -> \EFI\BOOT\BOOTKIT.EFI
efiboot: installed; press any key to reboot
```

Expected loader output (before the kernel takes over):

```
BdsDxe: loading Boot000N "MikroTikBootKit" ... FilePath(\EFI\BOOT\BOOTKIT.EFI)
BdsDxe: starting Boot000N "MikroTikBootKit" ...
efiboot: MikroTik boot kit loader
efiboot: kernel 5.6.3-64 (gitlab-runner@cicd-a13.mt.lv) #1 SMP ...
efiboot: initrd 86016 bytes, booting
```

Expected tracer output:

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
* The kit is added to the image's own ESP *by its installer* (self-copy to
  `\EFI\BOOT\BOOTKIT.EFI` plus the `Boot####` entry).  The stock kernel stays
  at `\EFI\BOOT\BOOTX64.EFI` (4,036,832/4,024,544 bytes depending on the
  version) and is what `efiboot.c` reads (compile-time constant at the top of
  the file); the initramfs is embedded in `bootkit.efi`.
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
