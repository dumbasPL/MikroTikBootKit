# Boot-time exec hijack with ptrace (`ptrace_init`)

Runtime alternative to patching the NPK: the initramfs init is replaced (or
wrapped) by a small binary that ptrace-attaches to the real init, follows the
fork that execs `/sbin/sysinit`, and redirects the `mode` exec back to itself
(`/proc/<tracer-pid>/exe`).  Nothing in the RouterOS image is modified, so the NPK signature stays
valid and no licence key is needed.

Source: `ptrace_init.c` (build: `./build.sh`).  Part of MikroTikBootKit.

## How it works

```
kernel -> /ptrace_init (PID 1)
    |-- fork -> tracer child
    |     PTRACE_SEIZE(PID 1) with TRACESYSGOOD|TRACEFORK|TRACEVFORK|
    |     TRACECLONE|TRACEEXEC|TRACEEXIT, then follow every fork/exec
    |       * execve("sysinit")  -> follow it (and its forks)
    |       * execve("mode")     -> patch the syscall's path argument to
    |         /proc/<tracer-pid>/exe
    |       * any other exec     -> detach that process (loader uses ptrace
    |         itself and must run untraced)
    |       * after the mode exec: detach everything and exit
    `-- execve("/init")   (the real init; boot proceeds normally)
```

When the hijacked process runs the payload, it writes
`/flash/rw/disk/flag.txt` (visible as `/file flag.txt`) and then
`execve("/nova/bin/mode")` so the real licence daemon runs as usual.  The
binary decides which role it is in by pid: PID 1 is the wrapper, anything else
is the payload exec'd as `mode`.

## Why each piece is needed

* **PTRACE_SEIZE, not PTRACE_ATTACH.**  SIGSTOP cannot be delivered to the
  global init (`sig_task_ignored()` drops `sig_kernel_only()` signals for
  `is_global_init()`), so `PTRACE_ATTACH` would never stop it.  SEIZE sets the
  options without sending a signal; the first stop is the `PTRACE_EVENT_EXEC`
  of the real init.
* **The tracer must be a child of the init** (fork before the exec) so that it
  can ptrace it with `CAP_SYS_PTRACE`; no namespaces are involved, so the
  tracee and the payload share the mount namespace.
* **Never detach the init.**  The stock init re-execs itself during the stage-1
  tmpfs move (`execve("/init")`); a naive "detach everything that execs
  something unrelated" rule lets go of PID 1 and the hijack never happens.
* **There are two sysinits.**  `boot_stage_mount_system` spawns
  `chroot_exec("/system", "/sbin/sysinit")` and `main_init`'s tail spawns
  another one after `pivot_root`; the *second* one is the one that forks
  `/nova/bin/mode` (observed: mode's parent is the second sysinit).  The
  tracer therefore keeps following the init (event-only, `PTRACE_CONT`) after
  the first sysinit so the second one is still caught.
* **Detach unrelated programs at their exec.**  `loader` imports `ptrace()`
  and does `PTRACE_ATTACH`/`PTRACE_CONT`/`waitpid` on its own children; if it
  ran under a tracer its checks/loops could misbehave.  Every process whose
  exec is neither `sysinit` nor `mode` is detached *at the execve entry*, so
  it continues into the new image untraced.
* **Memory is accessed only through ptrace** (`PTRACE_PEEKDATA` /
  `PTRACE_POKEDATA`, one word at a time) - no `/proc/<pid>/mem`, and the
  tracer does not need `/proc` mounted itself (the tracee's `/proc` is what
  matters).  A 32-bit tracer can only name 32-bit addresses, which is all
  RouterOS userspace needs.
* **One hijack point: the `mode` execve entry.**  The path string of the
  syscall itself is overwritten with `PTRACE_POKEDATA` to
  `/proc/<tracer-pid>/exe` (shorter than `"/nova/bin/mode"`; the tracee's
  `.rodata` is a private mapping, so the write is COW and never touches the
  file).  Nothing else in the process is touched and no memory is scanned.
  The tracee is a fresh fork of sysinit that has not exec'd anything yet
  (sysinit's spawn helper does `fork`, `open`/`dup2`, `execv("/nova/bin/mode")`),
  so it is still traced at that point.
* **Nothing is copied: the payload is the tracer's own exe.**
  `/proc/<tracer-pid>/exe` is a magic link to the file the tracer was started
  from (the initramfs, i.e. RAM), and the tracee resolves it in its own root:
  the stock init mounts procfs and moves it into the system root before
  sysinit - and therefore mode - runs, so `/proc` is there.  The kernel
  follows the link and execs that file; the new process keeps it alive.
  Nothing is written to `/ram` or to the flash (only the flag file).
* **The path has to fit.**  `"/proc/<pid>/exe"` is 10 + digits characters, so
  it only fits in `"/nova/bin/mode"` (14) while the tracer's pid is below
  10000 - at boot it is ~110 (three digits).  If it ever did not fit, the
  hijack is skipped and the real `mode` runs.
* **32-bit tracer.**  Built as a static i386 binary (musl) to match RouterOS
  userspace, so `PTRACE_PEEKDATA`/`POKEDATA` use 4-byte words and 32-bit
  addresses (checked before every access).  Tracing a 64-bit tracee would need
  a 64-bit tracer.

## Verified on 7.23.7 (CHR-mode x86 install, QEMU/KVM)

Serial log of a boot with `rdinit=/ptrace_init`:

```
[ptrace-init] tracing pid 1
[ptrace-init] exec /init
[ptrace-init] sysinit is pid 119
[ptrace-init] sysinit is pid 121
[ptrace-init] hijacked pid 129: /nova/bin/mode -> /proc/111/exe
[ptrace-init] pid 129 runs the payload, detaching
[ptrace-init] mode hijack: wrote /flash/rw/disk/flag.txt
Starting services...
MikroTik 7.23.7 (long-term)
CHR Login:
```

And from the RouterOS CLI:

```
[admin@CHR] > /file/tail flag.txt
 ptrace_init hijack: OK
 exe=/ptrace_init
 pid=129
 ppid=121
 time=1790194498

[admin@CHR] > /system/license print
  system-id: R+YnTj7FTmO
      level: free
```

The original `mode` runs normally after the payload (licence level `free` on a
CHR with no licence), and the boot is otherwise untouched.

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
A copy of the CHR-converted x86 install image (`x86-work.img` -> `x86-hijack.img`)
with the initrd added to its EFI system partition (p1 starts at LBA 0x800, so
`mtools` addresses it as `image@@1048576`):

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
  -drive if=none,id=d1,file=x86-hijack.img,format=raw \
  -device qemu-xhci,id=usb-bus \
  -device usb-storage,bus=usb-bus.0,drive=d1,serial=test \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
  -drive if=pflash,format=raw,file=vars.fd \
  -display none \
  -chardev socket,id=ser,path=serial.sock,server=on,wait=off,logfile=serial.log \
  -serial chardev:ser
```

Rebuild loop: `./build.sh`, re-make the cpio, `mcopy` it over
`::/initrd.cpio`, boot, then check the serial log for the `[ptrace-init]`
lines and `CHR Login:`, and read the flag back from the image
(`AGENTS.md` has a one-liner that prints the newest flag).

The paths are compile-time constants at the top of `ptrace_init.c`
(`ORIG_MODE`, `FLAG_PATH`, `REAL_INIT`); the payload path is built at runtime
(`/proc/<tracer-pid>/exe`).

## Notes

* The tracer only stops processes it still needs: the init (until the first
  sysinit is seen), the sysinit chain (until the mode exec), and any process
  that has not yet exec'd.  Everything else is detached at its first exec, so
  the boot keeps its normal timing.
* If the payload cannot be written, the path is left alone and the real
  `mode` runs — a failed hijack cannot break the boot.
* The hijack relies on mode being exec'd by a process the tracer still
  follows.  Verified: mode is exec'd by a fresh fork of sysinit (ppid = the
  second sysinit).  A process that execs something else first is detached (see
  `loader` above) and its later execs are not intercepted; if a future
  RouterOS version spawned mode through such an intermediate, either relax the
  detach rule or re-add the `sysinit`-image string patch (see git history of
  `ptrace_init.c`).
* The same trick can redirect any other exec (`keyman`, `loader`, ...); the
  match list is the `base` comparison in `ptrace_init.c`.
* The binary is ~52 KB static and writes nothing anywhere except the flag
  file; if the path cannot be patched (does not fit, or the write fails), it
  is left alone and the real `mode` runs.
