# MikroTikBootKit

Boot-time tooling for MikroTik RouterOS images.  Currently one tool:

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
ptrace_init.c               the tool (static i386)
preload.c                   LD_PRELOAD probe: console log, in-memory key patch,
                            embedded keygen glue
keygen.c                    embeddable licence keygen (adapted from MikroTikPatch)
build.sh                    builds + embeds the probe, then the tool
vmtest.sh                   prepare/boot/interact with a test image copy
vmconsole.py                serial-console helper used by vmtest.sh cmd
docs/ptrace-init-preload.md design + findings + verified log
```

## Build

```sh
./build.sh                  # -> ./ptrace_init (static i386, ~90 KB with the probe)
```

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

Needs: KVM (`-enable-kvm` is mandatory), `edk2-ovmf` + `edk2-shell` (or any
EFI shell binary), `mtools`, `cpio`, and a RouterOS x86 image.  Use a *copy*
of the image; the ESP (partition 1) is rewritten.  Always kill the VM by
pidfile when done — do not leave QEMU instances running.

`./vmtest.sh test` does all of the following on a copy (and
`./vmtest.sh cmd "<cli command>"` logs into the running VM as admin/admin):

```sh
# 1. build + initrd
./build.sh
rm -rf /tmp/pb-root && mkdir -p /tmp/pb-root
cp ptrace_init /tmp/pb-root/
(cd /tmp/pb-root && find . | cpio -o -H newc --owner=0:0) > /tmp/ptrace-init.cpio

# 2. test image: copy, make it CHR mode (no licence needed), add the ESP files
IMG=../.work/pb-test.img        # clone of the clean x86 image; keep it on real disk, not tmpfs
cp ../x86-7.23.7-clean.img $IMG
printf '\001' | dd of=$IMG bs=1 seek=$((0x150)) conv=notrunc status=none   # MBR mode flag -> CHR
mcopy -i $IMG@@1048576 ::/EFI/BOOT/BOOTX64.EFI /tmp/kernel.efi             # stock kernel, from the image itself
mcopy -i $IMG@@1048576 -o /usr/share/edk2-shell/x64/Shell.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i $IMG@@1048576 -o /tmp/kernel.efi                     ::/EFI/BOOT/KERNEL.EFI
mcopy -i $IMG@@1048576 -o /tmp/ptrace-init.cpio               ::/initrd.cpio
printf 'fs0:\\EFI\\BOOT\\kernel.efi console=ttyS0,115200n8 initrd=\\initrd.cpio rdinit=/ptrace_init\n' > /tmp/startup.nsh
mcopy -i $IMG@@1048576 -o /tmp/startup.nsh ::/startup.nsh

# 3. boot (EFI shell runs startup.nsh; initrd comes in via the EFI stub)
cp /usr/share/edk2/x64/OVMF_VARS.4m.fd /tmp/vars.fd
qemu-system-x86_64 -m 1024 -smp 2 -cpu host -enable-kvm \
  -drive if=none,id=d1,file=$IMG,format=raw \
  -device qemu-xhci,id=usb-bus \
  -device usb-storage,bus=usb-bus.0,drive=d1,serial=test \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
  -drive if=pflash,format=raw,file=/tmp/vars.fd \
  -display none -serial file:/tmp/serial.log -pidfile /tmp/qemu.pid

# 4. wait ~45 s, then check
grep -a ptrace-init /tmp/serial.log      # tracer messages
grep -a ldpreload /tmp/serial.log        # every binary that loaded the probe
tail -c 100 /tmp/serial.log              # should end with "CHR Login:"
kill $(cat /tmp/qemu.pid)
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
* **The loader is never touched.**  It also verifies licences, but RouterOS
  cross-checks its embedded key against other state at boot and a modified
  loader aborts the system supervisor (`/nova/bin/sys2`); the CHR boot gate
  does not need its key patched.  The probe only patches `mode` and `keyman`.
* **The licence check is CHR-tested only.**  On CHR the licence is bound to
  the VM UUID plus the software id; x86 mode has extra hardware checks and is
  not attempted yet (the embedded keygen still contains the x86 path, it is
  just unverified).
* **Do not boot with QEMU's `-kernel` + `-initrd`.**  It was tried: the
  wrapper runs, but the stock init then fails (`opendir: No such file or
  directory` → `ERROR: no system package found!`) and the kernel panics.  The
  disk is fine (`/dev/vda{,1,2}` present, `/dev/vda2` mounts), and the same
  image boots under OVMF — the init appears to need the EFI environment.  Use
  the EFI-shell route above.
* The ESP files must be *added* to the image's own ESP; the stock kernel is
  copied out of it first (`::/EFI/BOOT/BOOTX64.EFI`, 4,024,544 bytes) and put
  back as `KERNEL.EFI` because the shell's name has to be `BOOTX64.EFI`.
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
