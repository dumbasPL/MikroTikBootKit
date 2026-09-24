# Boot-time LD_PRELOAD drop with ptrace (`ptrace_init`)

Runtime alternative to patching the NPK: the initramfs init is replaced by a
small binary that ptrace-attaches to the real init and waits for it to mount
the tmpfs on `/ram`.  As soon as the mount succeeds the tracer writes an
embedded shared object to `/ram/ldpreload.so`, detaches the init and
exits.  The wrapper put `LD_PRELOAD=/ram/ldpreload.so` into the environment
before exec'ing `/init`, so every dynamically linked binary started after the
mount - `sysinit`, `mode`, `loader`, all services - loads the probe, and the
probe's constructor prints the binary's own name to `/dev/console`.  Nothing
in the RouterOS image is modified.

Source: `ptrace_init.c` (build: `./build.sh`).  Part of MikroTikBootKit.

## How it works

```
kernel -> /ptrace_init (PID 1)
    |-- fork -> tracer
    |     PTRACE_SEIZE(PID 1) with TRACESYSGOOD|TRACEEXEC, then watch the
    |     init's syscalls (no fork following - the init is the only tracee)
    |       * mount("tmpfs","/newroot",...) -> chroot into it (see below)
    |       * mount("tmpfs","/ram",...)     -> write /ram/ldpreload.so,
    |         detach the init and exit
    `-- execve("/init")   (the real init; LD_PRELOAD=/ram/ldpreload.so)
```

The probe (`preload.c`) is a plain i386 shared object built without libc
(`-nostdlib`): no `DT_NEEDED`, the imports (`open`/`write`/`readlink`/
`getpid`/`snprintf`/...) are resolved at load time by the dynamic linker
against the libc already in the process (RouterOS `/lib/libc.so`).  `build.sh`
embeds it in the init binary as a C array.  Its ELF constructor reads
`/proc/self/exe` and writes `[ldpreload] loaded by <path> (pid=<n>)` to
`/dev/console`.

## Why each piece is needed

* **PTRACE_SEIZE, not PTRACE_ATTACH.**  SIGSTOP cannot be delivered to the
  global init (`sig_task_ignored()` drops `sig_kernel_only()` signals for
  `is_global_init()`), so `PTRACE_ATTACH` would never stop it.  SEIZE sets the
  options without sending a signal; the first stop is the `PTRACE_EVENT_EXEC`
  of the real init.
* **The tracer must be a child of the init** (fork before the exec) so that it
  can ptrace it with `CAP_SYS_PTRACE`; no namespaces are involved, so the
  tracee and the tracer share the mount namespace.
* **Follow the init's stage-1 root switch.**  The stock init mounts a tmpfs on
  `/newroot`, moves that mount to `/` (`mount(..., MS_MOVE)`) and `chroot()`s
  into it.  A mount move does not change other processes' roots, so the tracer
  has to `chroot("/newroot")` itself - it does that at the syscall-exit stop
  of the tmpfs mount, while `/newroot` is still visible in its (old) root.
  Without this the tracer stays rooted in the old initramfs and `/ram` does
  not exist for it (the drop fails with `ENOENT`).
* **`/ram` is mounted early.**  `boot_stage_mount_system()` mounts the tmpfs on
  `/ram` (0x80525A4 in the 7.23.7 init) before it spawns `sysinit`, so the
  probe is already there when `sysinit`, `mode`, `loader` and the services are
  exec'd - earlier than the old exec-hijack approach, which only caught `mode`
  and everything after it.
* **Only the init is traced.**  It is the process that mounts the tmpfs on
  `/ram`, so forks/clones are not followed at all and the init's children
  (`sysinit`, `loader`, ...) are never traced.  That also removes the
  `loader`-uses-ptrace hazard: it only exists for a tracer that follows forks.
  The init's own re-exec (stage-1) is handled because it is the same pid.
* **Memory is accessed only through ptrace** (`PTRACE_PEEKDATA`, one word at a
  time) - no `/proc/<pid>/mem`; the syscall arguments (the mount strings) are
  read from the tracee this way.
* **32-bit tracer.**  Built as a static i386 binary (musl) to match RouterOS
  userspace, so `PTRACE_PEEKDATA` uses 4-byte words and 32-bit addresses.

## Verified on 7.23.7 (CHR-mode x86 install, QEMU/KVM)

Serial log of a boot with `rdinit=/ptrace_init`:

```
[ptrace-init] tracing pid 1
[ptrace-init] exec /init (LD_PRELOAD=/ram/ldpreload.so)
[ptrace-init] following the init into /newroot
[ptrace-init] pid 1 mounts tmpfs on /ram
[ptrace-init] dropped /ram/ldpreload.so (13636 bytes), detaching
[ldpreload] loaded by /sbin/sysinit (pid=119)
[ldpreload] loaded by /sbin/fsck.ext2 (pid=120)
[ldpreload] loaded by /sbin/sysinit (pid=121)
[ldpreload] loaded by /sbin/kexec (pid=122)
[ldpreload] loaded by /nova/bin/diskd (pid=123)
[ldpreload] loaded by /bin/catlog (pid=125)
[ldpreload] loaded by /nova/bin/sstore (pid=127)
[ldpreload] loaded by /nova/bin/mode (pid=129)
[ldpreload] loaded by /nova/bin/loader (pid=130)
... 47 loads in total; the boot reaches "CHR Login:" normally
```

## Usage

Build:

```sh
./build.sh                                        # -> ptrace_init (static i386)
```

Install: put the binary in the initramfs (an external initrd overlays the
built-in one) and boot with `rdinit=/ptrace_init`.  The stock initramfs is not
modified - `/init` stays the real init, which the wrapper execs unchanged.

## Test setup used here

The runnable recipe is in `../AGENTS.md`; this is the record of what was used.
A copy of the CHR-converted x86 install image with the initrd added to its EFI
system partition (p1 starts at LBA 0x800, so `mtools` addresses it as
`image@@1048576`):

```
/EFI/BOOT/BOOTX64.EFI   = EFI shell (runs startup.nsh)
/EFI/BOOT/KERNEL.EFI    = stock RouterOS kernel (EFI stub, 4024544 bytes)
/initrd.cpio            = cpio with /ptrace_init
/startup.nsh            = fs0:\EFI\BOOT\kernel.efi console=ttyS0,115200n8 \
                          initrd=\initrd.cpio rdinit=/ptrace_init
```

Booting QEMU's `-kernel BOOTX64.EFI -initrd ... -append rdinit=...` was tried
first and does not work for this image: the wrapper runs, but the stock init
then fails with `opendir: No such file or directory` /
`ERROR: no system package found!` and the kernel panics.  The disk itself is
fine (a diagnostic init confirmed `/dev/vda`, `vda1`, `vda2` and a working
`mount("/dev/vda2","/flash","ext4",MS_RDONLY)`), and the same image boots via
OVMF, so the init needs the EFI environment to find the package.  Hence the
EFI-shell route.

```
qemu-system-x86_64 -m 1024 -smp 2 -cpu host -enable-kvm \
  -drive if=none,id=d1,file=x86-preload.img,format=raw \
  -device qemu-xhci,id=usb-bus \
  -device usb-storage,bus=usb-bus.0,drive=d1,serial=test \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
  -drive if=pflash,format=raw,file=vars.fd \
  -display none \
  -chardev socket,id=ser,path=serial.sock,server=on,wait=off,logfile=serial.log \
  -serial chardev:ser
```

Rebuild loop: `./build.sh`, re-make the cpio, `mcopy` it over
`::/initrd.cpio`, boot, then check the serial log for the `[ptrace-init]` and
`[ldpreload]` lines and for `CHR Login:`.

The paths are compile-time constants at the top of `ptrace_init.c`
(`REAL_INIT`, `PRELOAD_PATH`); the mount strings the tracer matches
(`/newroot`, `/ram`, `tmpfs`) are stock-init behaviour - re-check them when
moving to another RouterOS version.

## Notes

* The tracer only ever traces the init.  Children it forks run untraced, and
  after the drop the init is detached and the tracer exits.
* If the mount is never seen (or the drop fails), the boot is unaffected; the
  probe simply never loads.  A failed `chroot("/newroot")` or a failed drop is
  logged and the tracer keeps going.
* The probe only loads in dynamically linked binaries; static binaries (the
  init itself, the tracer) never run its constructor.  The binaries exec'd
  before the drop (`/init`) do not load it either.
