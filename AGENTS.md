# MikroTikBootKit

Boot-time tooling for MikroTik RouterOS images.  Currently one tool:

* **`ptrace_init.c`** — an alternative initramfs init (`rdinit=/ptrace_init`)
  that ptrace-attaches to the real init, follows the fork that execs
  `/sbin/sysinit`, and redirects the `execve` of `/nova/bin/mode` to
  `/proc/<tracer-pid>/exe` (itself).  The resulting "mode" process writes
  `/flash/rw/disk/flag.txt` and then execs the real licence daemon.  Nothing
  in the RouterOS image is modified.

Design notes, the target behaviours it relies on and the observed boot log:
`docs/ptrace-init-hijack.md`.

## Layout

```
ptrace_init.c               the tool (static i386)
build.sh                    build script
docs/ptrace-init-hijack.md  design + findings + verified log
```

## Build

```sh
./build.sh                  # -> ./ptrace_init (~51 KB static i386)
```

It uses, in order: a local `.toolchain/i386-musl`, MikroTikPatch's
`.toolchain/i386-musl` (create it with `MikroTikPatch/tools/musl_i386.sh`), or
a multilib host `gcc -m32`.  The binary is static, so it runs in the
initramfs with no libraries.

## Test

Needs: KVM (`-enable-kvm` is mandatory), `edk2-ovmf` + `edk2-shell` (or any
EFI shell binary), `mtools`, `cpio`, and a RouterOS x86 image.  Use a *copy*
of the image; the ESP (partition 1) is rewritten.  Always kill the VM by
pidfile when done — do not leave QEMU instances running.

```sh
# 1. build + initrd
./build.sh
rm -rf /tmp/pb-root && mkdir -p /tmp/pb-root
cp ptrace_init /tmp/pb-root/
(cd /tmp/pb-root && find . | cpio -o -H newc --owner=0:0) > /tmp/ptrace-init.cpio

# 2. test image: copy, make it CHR mode (no licence needed), add the ESP files
IMG=../.work/pb-test.img        # ~1 GB; keep it on real disk, not tmpfs
cp ../x86-7.23.7.img $IMG
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
tail -c 100 /tmp/serial.log              # should end with "CHR Login:"
python3 -c "import re; d=open('$IMG','rb').read(); m=[x.group(0) for x in re.finditer(rb'ptrace_init hijack: OK\nexe=/ptrace_init\npid=\d+\nppid=\d+\ntime=\d+\n', d)]; print(m[-1].decode() if m else 'no flag found')" 
kill $(cat /tmp/qemu.pid)
```

Expected tracer output:

```
[ptrace-init] tracing pid 1
[ptrace-init] exec /init
[ptrace-init] sysinit is pid 120
[ptrace-init] sysinit is pid 122
[ptrace-init] hijacked pid 130: /nova/bin/mode -> /proc/112/exe
[ptrace-init] pid 130 runs the payload, detaching
[ptrace-init] all tracees detached, tracer exits
[ptrace-init] mode hijack: wrote /flash/rw/disk/flag.txt
```

### Notes / gotchas

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
  in as `admin` with an empty password, then `/file/tail flag.txt` and
  `/system/license print` (should still show the normal level).  The first
  login walks through a forced password change — skipping it with Ctrl-C
  worked once but later logins on that image failed, so use a fresh image
  copy if you need the CLI.
* `docs/ptrace-init-hijack.md` lists the target behaviours the logic depends
  on (two sysinits, the init's stage-1 re-exec, `loader`'s own ptrace use).
  Re-read it before "simplifying" the tracer loop.
