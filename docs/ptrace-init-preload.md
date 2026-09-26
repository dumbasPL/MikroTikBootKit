# Boot-time LD_PRELOAD drop with ptrace (`ptrace_init`)

Runtime alternative to patching the NPK: the initramfs init is replaced by a
small binary that ptrace-attaches to the real init and waits for it to mount
the tmpfs on `/ram`.  As soon as the mount succeeds the tracer makes an
embedded shared object available as `/ram/ldpreload.so`, detaches the init
and exits.  The wrapper put `LD_PRELOAD=/ram/ldpreload.so` into the
environment before exec'ing `/init`, so every dynamically linked binary
started after the mount - `sysinit`, `mode`, `loader`, all services - loads
the probe, and the probe's constructor prints the binary's own name to
`/dev/console`.  Nothing in the RouterOS image is modified.

The probe reaches `/ram` by **bind mount** from a copy on the initramfs
(rootfs).  This is not cosmetic: the 7.24.4 kernel refuses `PROT_EXEC`
mappings of tmpfs files, so a plain byte copy on `/ram` would never load
there; a rootfs inode stays exec-mappable even when bind-mounted onto the
tmpfs.  A failure to make any of the mounts is logged; the boot itself is
unaffected and simply runs without the probe.

Because the probe runs inside every service it is also the vehicle for the
licence work: in `/nova/bin/mode` it runs the embedded keygen in-process
(signing the licence blob with the custom key pair), in `mode` and `keyman` it
replaces the stock licence public key in that process's own text, and in
`/nova/bin/loader` it redirects the `memcmp` GOT slot to an "always equal"
stub (the reference tool's trick) so the loader's verifier accepts the custom
signature.  See
[Runtime licence: keygen + key patches](#runtime-licence-keygen--key-patches).

Source: `bootkit/ptrace_init.c` (build: `./build.sh`).  Part of MikroTikBootKit.

## How it works

```
kernel -> /ptrace_init (PID 1)
    |-- write the embedded probe to /ptrace_init.so (initramfs/rootfs)
    |   and keep it open
    |-- fork -> tracer
    |     PTRACE_SEIZE(PID 1) with TRACESYSGOOD|TRACEEXEC, then watch the
    |     init's syscalls (no fork following - the init is the only tracee)
    |       * mount("tmpfs","/newroot",...) -> chroot into it (see below)
    |       * mount("tmpfs","/ram",...)     -> bind /proc/self/fd/N over
    |         /ram/ldpreload.so, detach the init and exit
    `-- execve("/init")   (the real init; LD_PRELOAD=/ram/ldpreload.so)
```

The bind source is the tracer's inherited fd to the rootfs file
(`/proc/self/fd/N`), so it works even after the initramfs is detached.  The
`/ram` mount is the one the init moves into the final root, which is why the
probe is placed there: it is reachable as `/ram/ldpreload.so` by every
service, and the bind keeps the rootfs backing that makes it mappable on
7.24.4.

The probe (`bootkit/preload.c`) is a plain i386 shared object built without libc
(`-nostdlib`): no `DT_NEEDED`, the imports (`open`/`write`/`readlink`/
`getpid`/`snprintf`/...) are resolved at load time by the dynamic linker
against the libc already in the process (RouterOS `/lib/libc.so`).  `build.sh`
embeds it in the init binary as a C array.  Its ELF constructor reads
`/proc/self/exe` and writes `[ldpreload] loaded by <path> (pid=<n>)` to
`/dev/console`; for `mode`, `keyman` and `loader` it additionally patches the
licence (keygen, public key, memcmp GOT), see
[Runtime licence: keygen + key patches](#runtime-licence-keygen--key-patches).

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
* **The probe is bind-mounted, not copied.**  `mount(2)` with `MS_BIND`
  keeps the source inode, so the file on the initramfs (rootfs) can be
  reached at `/ram/ldpreload.so` while staying rootfs-backed.  On 7.24.4 the
  kernel refuses `PROT_EXEC` mappings of tmpfs files but allows rootfs files;
  a byte copy on `/ram` would therefore never load there.  The bind source is
  the tracer's inherited fd (`/proc/self/fd/N`), so it does not depend on the
  initramfs still being reachable by path.  `/ram` is used as the target
  because it is the mount the init moves into the final root; a file parked
  in the stage-1 root would be detached with it at `pivot_root`.  See the
  7.24.4 section below for the mapping matrix.
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

## Verified on 7.23.7 and 7.24.4 (CHR-mode x86 install, QEMU/KVM)

Serial log of a boot with `rdinit=/ptrace_init` (current build):

```
[ptrace-init] tracing pid 1
[ptrace-init] exec /init (LD_PRELOAD=/ram/ldpreload.so)
[ptrace-init] following the init into /newroot
[ptrace-init] pid 1 mounts tmpfs on /ram
[ptrace-init] bind-mounted /proc/self/fd/3 -> /ram/ldpreload.so, detaching
[ldpreload] loaded by /sbin/sysinit (pid=119)
[ldpreload] loaded by /sbin/fsck.ext2 (pid=120)
[ldpreload] loaded by /sbin/sysinit (pid=121)
[ldpreload] loaded by /sbin/kexec (pid=122)
[ldpreload] loaded by /nova/bin/diskd (pid=123)
[ldpreload] loaded by /bin/catlog (pid=125)
[ldpreload] loaded by /nova/bin/sstore (pid=127)
[ldpreload] loaded by /nova/bin/mode (pid=129)
[ldpreload] loaded by /nova/bin/loader (pid=130)
... 42-47 loads in total; the boot reaches "CHR Login:" normally
```

The same build reaches the login prompt with 42-45 loads on 7.24.4.  The
older build, which wrote the probe bytes to `/ram`, logged
`dropped /ram/ldpreload.so (13636 bytes), detaching` instead; it does not
load on 7.24.4, which is why the bind-mount route replaced it.

## Usage

Build:

```sh
./build.sh                        # production build:
                                  #    ptrace_init (static i386)
                                  #    bootkit.efi (EFI loader, see bootloader/)
                                  #    bootkit.img (32 MB USB stick image)
DEBUG=1 ./build.sh                # test build: serial console + verbose logs
EFI_CC=x86_64-w64-mingw32-gcc ./build.sh   # build the loader with gcc
```

The i386 binary is always built against musl; when `.toolchain/i386-musl` is
missing, `build.sh` runs `tools/musl_i386.sh` (multilib host gcc + network)
by itself.

Install: `./build.sh` packs the binary into a cpio and embeds that in the EFI
loader (`bootkit.efi`), which boots it with `rdinit=/ptrace_init`.  The stock
initramfs is not modified - `/init` stays the real init, which the wrapper
execs unchanged.  `bootkit.img` is an MBR disk with one FAT EFI system
partition holding the loader as `\EFI\BOOT\BOOTX64.EFI`; flash it to a USB
stick (`dd if=bootkit.img of=/dev/sdX bs=4M conv=fsync`) and boot the router
from it to run the install menu.

## Test setup used here

The runnable recipe is in `../AGENTS.md`; this is the record of what was used.
A copy of the CHR-converted x86 install image with the bootkit added to its EFI
system partition (p1 starts at LBA 0x800, so `mtools` addresses it as
`image@@1048576`):

```
/EFI/BOOT/BOOTKIT.EFI   = bootkit EFI loader (bootloader/ -> bootkit.efi),
                          with the /ptrace_init cpio embedded; written and
                          given its Boot#### entry by the loader's installer
                          (direct install)
/EFI/BOOT/BOOTX64.EFI   = stock RouterOS kernel (EFI stub, ~4 MB), left in
                          place so RouterOS updates keep overwriting it
/BOOTKIT.CFG            = target ESP identity (the partition RouterOS lives
                          on), written by the installer
```

The loader copies the embedded cpio below 4 GB, fills in `struct boot_params`
(kernel command line, ramdisk address and size) and jumps to the kernel's EFI
handover entry.  The entry point is the kernel's own EFI stub, which relocates
the kernel, exits boot services and jumps into the decompressor - so the
kernel still sees a full EFI environment.  Earlier the ESP ran an EFI shell +
`startup.nsh` instead; the EFI loader replaced that (the log below is unchanged
by it), and the stock kernel used to be renamed to `KERNEL.EFI` because the
loader had to be `BOOTX64.EFI`.  With the loader under its own name the kernel
file is never touched.

On boot the loader reads `\BOOTKIT.CFG` next to itself.  It names the ESP
RouterOS is installed on by the HD() device path identity (signature, start,
size - not the bus topology); the loader finds that partition and boots
`\EFI\BOOT\BOOTX64.EFI` from it.  The `debug=0|1` line in the config
controls the serial console (`console=ttyS0,115200n8` is appended to the
kernel command line only with `debug=1`) and the verbosity of the tracer and
probe logs: production prints only the important probe lines (licence state
and the three patches) and errors, debug=1 prints every step.  The installer
writes the build default (`DEBUG=1 ./build.sh` -> `debug=1`); edit the line
and reboot to switch without rebuilding.

Without a valid config it runs the install menu instead: pick the RouterOS
ESP, then `direct` (copy the loader + config to the target, create the
`Boot####` entry) or `removable` (write only `\BOOTKIT.CFG` next to the
loader, e.g. on a USB stick, and leave the target untouched).  `--install`
forces the menu.

Booting QEMU's `-kernel BOOTX64.EFI -initrd ... -append rdinit=...` was tried
first and does not work for this image: the wrapper runs, but the stock init
then fails with `opendir: No such file or directory` /
`ERROR: no system package found!` and the kernel panics.  The disk itself is
fine (a diagnostic init confirmed `/dev/vda`, `vda1`, `vda2` and a working
`mount("/dev/vda2","/flash","ext4",MS_RDONLY)`), and the same image boots via
OVMF, so the init needs the EFI environment to find the package.  Hence the
EFI-boot route.

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

The one-time install run adds the installer stick as a second USB drive
(`id=d2,serial=bk-stick`); the target's `\EFI\BOOT\BOOTX64.EFI` is moved to
`BOOTX64.BAK` for that run only so the stick boots, and restored afterwards
(`vmtest.sh install` does all of it).  For the removable boot phase both disks
are attached with `bootindex=0` on the stick so OVMF starts it first.

Rebuild loop: `./build.sh` (it re-packs `ptrace_init` and re-embeds it in
`bootkit.efi`), `mcopy` `bootkit.efi` over `::/EFI/BOOT/BOOTKIT.EFI` (the
config, boot entry and kernel stay untouched), boot, then check the serial log
for the `[ptrace-init]` and `[ldpreload]` lines and for `CHR Login:`.  To see
the install menu again, delete `\BOOTKIT.CFG` or start the loader with
`--install`.

The paths are compile-time constants at the top of `bootkit/ptrace_init.c`
(`REAL_INIT`, `PRELOAD_PATH`); the mount strings the tracer matches
(`/newroot`, `/ram`, `tmpfs`) are stock-init behaviour - re-check them when
moving to another RouterOS version.

## Test harness (`vmtest.sh`, `vmconsole.py`)

`vmtest.sh` automates everything above on a *copy* of an image (the source
image is never touched):

```sh
./vmtest.sh prepare              # build, copy the image, use bootkit.img as the stick
./vmtest.sh iso-install          # install a clean image from an installer ISO
./vmtest.sh install              # boot stick + target and drive the installer
./vmtest.sh boot                 # start QEMU (background, serial on a socket)
./vmtest.sh boot-removable       # boot stick + target (stick first)
./vmtest.sh wait 60              # wait for the login prompt
./vmtest.sh check                # show the loader + tracer + ldpreload lines
./vmtest.sh cmd "/system license print"   # log in (admin/admin) and run one CLI command
./vmtest.sh login                # attach to the serial console (Ctrl-] quits)
./vmtest.sh stop                 # kill the VM
./vmtest.sh test                 # prepare + install + boot + wait + check
./vmtest.sh test-removable       # same, with the removable install mode
```

Environment knobs: `IMG_SRC` (source image, default
`x86-7.24.4-clean.img`; an `.iso` is installed to `$WORK/clean.img` first),
`ISO_SRC` / `ISO_OUT` / `IMG_SIZE` (the `iso-install` ISO, output image and
its size), `WORK` (scratch dir, default `.work/` next to the script),
`MODE=chr|x86|keep` (override the MBR mode flag), `INSTALL_MODE=1|2` (direct or
removable), `DEBUG=0|1` (build variant, harness default 1), `MEM`, `SMP`.
`vmconsole.py` is the serial helper used by `vmtest.sh cmd` (logs in as
admin/admin, declines the forced password change and runs the command; a
never-booted image is detected and set up to admin/admin on the fly), by
`vmtest.sh install` (`--install <mode>`: picks the non-installer ESP, picks
the mode, confirms and reboots) and by `vmtest.sh iso-install`
(`--ros-install`: takes the stock installer's default package selection,
confirms the disk wipe and waits for the install to finish; `--ros-firstboot`:
runs the first login of the new image, answers the licence question with "y",
quits the agreement pager with q + Enter and sets admin/admin).

## 7.24.4 denies PROT_EXEC mmaps of tmpfs files - solved with a bind mount

Verified on the stock 7.24.4 image (md5 of its kernel and of the ISO's
`isolinux/linux` are identical: `bd5305cb4887cadc1a6b3a2f7f13e254`): the
tracer runs and drops `/ram/ldpreload.so` normally, every service inherits
`LD_PRELOAD=/ram/ldpreload.so`, and the dynamic loader even **opens**
`/ram/ldpreload.so` successfully - but the library never appears in any
process's maps and no constructor runs.

Syscall tracing the ldso (`mmap2` with `PROT_READ|PROT_EXEC`,
`MAP_PRIVATE|MAP_FIXED`, offset 0x1000 into the probe) shows the kernel
rejects the mapping with `ENOTDIR` (errno 20).  A standalone probe (boot the
same kernel with a tiny `rdinit=` that calls `mmap` directly) reproduces it
without any RouterOS code:

| mapping                                   | 7.23.7 | 7.24.4 |
|---|---|---|
| anonymous, `PROT_READ\|PROT_EXEC`          | ok     | **ENOTDIR** |
| anonymous, `PROT_EXEC` only                | ok     | **ENOTDIR** |
| anonymous RW then `mprotect(RX)`           | ok     | ok      |
| tmpfs file, `PROT_READ\|PROT_EXEC`         | ok     | **ENOTDIR** |
| ramfs file, `PROT_READ\|PROT_EXEC`         | ok     | **ENOTDIR** |
| rootfs/initramfs file, RX                  | ok     | ok      |
| squashfs file (the mounted NPK), RX        | ok     | ok      |
| ext4 file on the system disk, RX            | ok     | **ENOTDIR** |

So on 7.24.4 the kernel only allows `PROT_EXEC` mappings from the squashfs
system image and from the initramfs rootfs; tmpfs, ramfs, ext4 and anonymous
exec mappings are refused.  That is what breaks the plain byte-copy drop: the
probe lands on the `/ram` tmpfs, the ldso cannot map it and silently
continues.  (The same restriction affects any other `LD_PRELOAD`/JIT/
memfd-exec mechanism that maps code out of tmpfs.)

### The fix: bind the rootfs copy over the tmpfs path

The restriction follows the **backing filesystem of the inode**, not the path
or mount point where the file is reached.  A `mount(2)` `MS_BIND` of a file
keeps the source inode, so a copy of the probe that lives on the initramfs
(rootfs) can be bind-mounted over `/ram/ldpreload.so` and stays
exec-mappable.  Verified in-guest on 7.24.4 with a dedicated `rdinit=`
(`mmmini3`):

| file reached through                    | backing fs | 7.24.4 RX map |
|---|---|---|
| `/sub/f.bin`                            | rootfs     | ok            |
| `/mnt/bindroot.bin` (bind of rootfs)    | rootfs     | **ok**        |
| `/bindtmp.bin` (bind of tmpfs)          | tmpfs      | ENOTDIR       |
| `/mnt/t.bin`                            | tmpfs      | ENOTDIR       |

The bootkit now uses this:

1. Before exec'ing the real init, PID 1 writes the embedded probe to
   `/ptrace_init.so` on the **initramfs** and keeps it open
   (`stash_preload()`); the tracer inherits the fd.
2. When the init mounts tmpfs on `/ram`, the tracer creates
   `/ram/ldpreload.so` and bind-mounts `/proc/self/fd/N` over it
   (`drop_preload()`), then detaches.  The bind keeps the rootfs inode, and
   `/ram` is moved into the final root by the init, so every service reaches
   the probe at `/ram/ldpreload.so`.

A file parked in the stage-1 root would *not* work: the init detaches that
root at `pivot_root` + `umount2(MNT_DETACH)`, and the services are chrooted
into the squashfs system root.  (Tested: `LD_PRELOAD=/ptrace-home.so` with
the staging bind gives 0 loads; the `/ram` bind is what carries the file
into the final root.)

Every mount is checked: a failure is logged with `strerror(errno)` and the
boot continues without the probe (the same behaviour as a failed drop had
before).  Serial log of a working run:

```
[ptrace-init] pid 1 mounts tmpfs on /ram
[ptrace-init] bind-mounted /proc/self/fd/3 -> /ram/ldpreload.so, detaching
... 42 loads on 7.24.4, boot reaches CHR Login:
```

Likely cause of the kernel behaviour: a hardening patch in the 7.24.4 kernel
to its `mmap`/`file_operations` path (the kernels are stripped, so this was
established behaviourally, not from source).

## Runtime licence: keygen + key patches

The probe does more than log when it is loaded into the licence components.
This is the runtime equivalent of patching the NPK (patching the key
material in `mode`/`keyman`, installing the keygen as `mode`) plus the
recovered reference tool's loader trick - but nothing is written to the
image; the changes live in those processes and in the 512-byte licence blob.

* **`/nova/bin/mode`** - the embedded keygen runs first: `preload.c`
  `#include`s `keygen.c`, the standalone keygen trimmed to the
  generation role (CLI, mode2 hand-over and self test removed; no `exit()`;
  `kg_generate()`/`kg_error()` API), and calls it in the constructor, i.e.
  before `mode`'s `main` can read the blob.  It generates the software id if
  needed, derives the licence value (CHR: `MT_SHA256(swapped UUID || swid)[0:8]
  || 00 57 86 f4 03 00 00 00`), signs it with the custom private key and
  writes the blob through `/dev/flash` / `/dev/root-disk`, exactly like the
  keygen installed as `/nova/bin/mode` does.  `stored_licence_valid()` keeps
  an already-installed licence, so there is no re-sign/reboot loop; the
  constructor logs `licence generated` or `licence already installed` with the
  System ID to `/dev/console`, and failures with `kg_error()`.
* **`/nova/bin/mode` and `/nova/bin/keyman`** - the stock licence public key
  is replaced in the process's own text.  On i386 the key is not stored as a
  byte string: the verifier builds it on the stack with eight
  `mov dword [ebp-x], imm32` instructions, so the 4-byte key chunks sit in
  `.text` with up to 6 opcode bytes between them (`7.23.7 mode` at file
  offset `0x5e23`, `keyman` at `0x765e`; the 7.24.4 binaries are identical).
  The probe scans the executable's `r-xp` mapping from `/proc/self/maps`,
  matches the chunks with the same greedy gap search as the reference key
  patcher and rewrites the chunks in place, leaving the
  instruction bytes alone.  Only the private (COW) mapping is written.
* **`/nova/bin/loader`** - its `memcmp` GOT slot is redirected to a stub that
  always returns 0.  The loader's licence verifier ends in
  `memcmp(MT_SHA256(y.x), witness, 16) == 0`, so the redirect makes it accept
  any blob - including the custom signature its stock key cannot verify.
  This is the trick the recovered reference tool uses (its stub patches the
  same GOT entry, `0x805d014` in 7.23.7).  The slot is found from the
  executable's own `.rel.plt` (`DT_JMPREL`) and `.rel.dyn` tables plus
  dynsym/dynstr, so no address is hard-coded; only the one GOT word is
  written.  The loader's text, rodata and key material are left untouched,
  which matters: changing those makes the system supervisor
  (`/nova/bin/sys2`) abort.

The pages are made writable for the patch.  7.24.4 rejects
`mprotect(PROT_READ|PROT_WRITE|PROT_EXEC)` (its W^X hardening), so the probe
asks for `rw` and restores `r-x` afterwards; the first 7.24.4 run showed
`licence key patched (0 site)` until that was fixed.

The key pair and the stock key are hard-coded in `keygen.c`/`preload.c` and
can be overridden at build time with the `CUSTOM_LICENSE_PUBLIC_KEY` /
`CUSTOM_LICENSE_PRIVATE_KEY` / `MIKRO_LICENSE_PUBLIC_KEY` environment
variables, so the signature the keygen makes verifies against the key the
probe patches in.

Verified on CHR-mode x86 installs (`MODE=chr`), 7.23.7 and 7.24.4, first boot
and reboot (the blob persists in sector 0):

```
[ldpreload] loaded by /nova/bin/mode (pid=129)
[ldpreload] mode: licence key patched (1 site)
[ldpreload] mode: licence generated (system-id d7qGpgIOFXP)
[ldpreload] loaded by /nova/bin/keyman (pid=135)
[ldpreload] keyman: licence key patched (1 site)

[admin@CHR] > /system license print
         system-id: d7qGpgIOFXP
             level: p-unlimited

# after the reboot:
[ldpreload] mode: licence already installed (system-id d7qGpgIOFXP)
```

Notes on scope:

* The **loader's key material is deliberately not patched.**  RouterOS
  cross-checks the loader's embedded key against other state; changing it
  aborts the supervisor (`/nova/bin/sys2`).  Its `memcmp` GOT slot is
  redirected instead (see above), which makes its verifier accept the custom
  signature without touching any text or key bytes.
* **x86 (non-CHR) mode works too** - tested on the 7.23.7 and 7.24.4 x86
  installs.  The keygen writes a freshly generated software id out first
  (keyman re-reads the blob for `--software-id`), takes the serial from
  keyman, signs the x86 licence value and mode reports `nlevel: 6` /
  `features: extra-channels`; the software id is stable across reboots
  ("licence already installed").

  Without the loader `memcmp` redirect the CLI still showed a rolling
  `expires-in` of ~72 h and the counter at `0x10C` ticked: the loader could
  not verify the custom signature and kept its demo/uptime state (its gate
  passed on the payload's hardware binding, and a licence whose signature
  bytes were corrupted still booted - mode re-signed it).  With the redirect
  the loader's verifier always succeeds: x86 shows no `expires-in`, the
  uptime counter stays at 0 and the one-shot `ROUTER HAS NEW SOFTWARE KEY`
  activation message is not raised.

## Notes

* The tracer only ever traces the init.  Children it forks run untraced, and
  after the drop the init is detached and the tracer exits.
* If the mount is never seen (or the drop fails), the boot is unaffected; the
  probe simply never loads.  A failed `chroot("/newroot")` or a failed drop is
  logged and the tracer keeps going.
* The probe only loads in dynamically linked binaries; static binaries (the
  init itself, the tracer) never run its constructor.  The binaries exec'd
  before the drop (`/init`) do not load it either.
