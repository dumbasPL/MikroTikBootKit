# MikroTikBootKit build
#
#   make             build both architectures: the x86_64 pair (ptrace_init,
#                    bootkit.efi, bootkit.img), the arm64 CHR pair
#                    (ptrace_init-arm, bootkit-arm64.efi, bootkit-arm64.img)
#                    and bootkit-auto.img (both loaders + a target=auto
#                    \BOOTKIT.CFG); arm64 and bootkit-auto.img are skipped
#                    with a warning when clang + lld-link or the ARM cross
#                    compiler are not installed
#   make x86         only the x86_64 pair
#   make arm64       only the arm64 CHR pair
#   make clean       remove the generated artifacts and .build/
#
# Variables (from the environment or the command line):
#   DEBUG=0|1        default 0; 1 = test build: the serial console and the
#                    verbose tracer/probe logs become the build default
#   EFI_CC=<cc>      x86 EFI loader compiler: clang (needs lld-link) or a
#                    PE-targeting gcc such as x86_64-w64-mingw32-gcc;
#                    default: auto (clang when present, else the mingw gcc)
#   ARM_CC=<cc>      host cross compiler for the arm64 CHR
#                    (default: arm-linux-gnueabihf-gcc)
#   USB_MB=<n>       stick image size in MB (default 32)
#   OUT=<path>       x86 ptrace_init output path (default: ./ptrace_init)
#   CUSTOM_LICENSE_PUBLIC_KEY / CUSTOM_LICENSE_PRIVATE_KEY /
#   MIKRO_LICENSE_PUBLIC_KEY
#                    override the embedded licence key material
#
# The init binaries and the probe are always built against musl; when
# .toolchain/ is missing (or tools/musl_*.sh changed) the matching script runs
# automatically (the i386 one needs a multilib host gcc, the arm one a host
# arm-linux-gnueabihf-gcc; both fetch musl if it is not cached).  To wipe the
# toolchains too: rm -rf .toolchain.
#
# .build/flags records DEBUG, the licence keys and the compiler choices, and
# .build/imgflags records USB_MB: the artifacts and the stick images depend on
# the matching stamp, so changed settings force a rebuild and unchanged ones
# do not.

ROOT     := $(patsubst %/,%,$(dir $(realpath $(firstword $(MAKEFILE_LIST)))))
MAKEFILE := $(realpath $(firstword $(MAKEFILE_LIST)))
TOOLS    := $(ROOT)/tools
BUILD    := $(ROOT)/.build
FLAGS    := $(BUILD)/flags
IMGFLAGS := $(BUILD)/imgflags

DEBUG  ?= 0
EFI_CC ?= auto
ARM_CC ?= arm-linux-gnueabihf-gcc
USB_MB ?= 32
OUT    ?= $(ROOT)/ptrace_init

TOOLCHAIN_I386 := $(ROOT)/.toolchain/i386-musl
TOOLCHAIN_ARM  := $(ROOT)/.toolchain/arm-musl
I386_CC        := $(TOOLCHAIN_I386)/bin/musl-gcc
ARM_MUSL_CC    := $(TOOLCHAIN_ARM)/bin/musl-gcc
ARM_INC        := $(TOOLCHAIN_ARM)/include

X86_EFI_NAMES   := efi_main efi boot config vars installer
X86_EFI_SRCS    := $(addprefix bootloader/,$(addsuffix .c,$(X86_EFI_NAMES)))
ARM64_EFI_NAMES := efi_main efi boot_arm64 initrdvol config vars installer
ARM64_EFI_SRCS  := $(addprefix bootloader/,$(addsuffix .c,$(ARM64_EFI_NAMES)))
# the loader headers; the generated initrd_so*.h are excluded here and
# attached explicitly to the loader that includes each one
LOADER_HDRS     := $(filter-out bootloader/initrd_so.h bootloader/initrd_so_arm.h,$(wildcard bootloader/*.h))

# the licence key pair can be overridden at build time; the probe and the
# keygen must use the same pair, so this applies to both architectures
DEFS := -DBOOTKIT_DEBUG_DEFAULT=$(DEBUG)
ifneq ($(strip $(CUSTOM_LICENSE_PUBLIC_KEY)),)
DEFS += -DKEYGEN_LICENSE_PUBLIC_HEX=\"$(CUSTOM_LICENSE_PUBLIC_KEY)\"
endif
ifneq ($(strip $(CUSTOM_LICENSE_PRIVATE_KEY)),)
DEFS += -DKEYGEN_LICENSE_PRIVATE_HEX=\"$(CUSTOM_LICENSE_PRIVATE_KEY)\"
endif
ifneq ($(strip $(MIKRO_LICENSE_PUBLIC_KEY)),)
DEFS += -DSTOCK_LICENSE_PUBLIC_HEX=\"$(MIKRO_LICENSE_PUBLIC_KEY)\"
endif

ifneq ($(filter $(DEBUG),0 1),)
else
$(error DEBUG must be 0 or 1)
endif

# the x86 EFI loader compiler: clang + lld-link is preferred (a gcc that
# targets PE, e.g. x86_64-w64-mingw32-gcc, works equally well)
ifeq ($(EFI_CC),auto)
EFI_CC := $(shell \
    if command -v clang >/dev/null 2>&1 && command -v lld-link >/dev/null 2>&1; then echo clang; \
    elif command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo x86_64-w64-mingw32-gcc; \
    fi)
endif

# "make"/"all" skips arm64 when its host tools are missing (explicit
# "make arm64" fails instead)
ifeq ($(shell command -v clang >/dev/null 2>&1 && command -v lld-link >/dev/null 2>&1 && command -v $(ARM_CC) >/dev/null 2>&1 && echo 1),1)
ARM64_HOST_OK := 1
endif

# flags for the chosen x86 EFI compiler
ifneq ($(findstring clang,$(EFI_CC)),)
X86_EFI_CCFLAGS := --target=x86_64-unknown-windows -ffreestanding -fno-stack-protector \
    -mno-red-zone -mno-sse -fshort-wchar -Os -Wall -Wextra -nostdlib \
    -fuse-ld=lld-link -Wl,/subsystem:efi_application,/entry:efi_main,/nodefaultlib
else
X86_EFI_CCFLAGS := -ffreestanding -fshort-wchar -fno-stack-protector -mno-red-zone \
    -mno-sse -fno-asynchronous-unwind-tables -Os -Wall -Wextra
X86_EFI_LDFLAGS := -nostdlib -Wl,--subsystem,10 -Wl,-e,efi_main -Wl,--no-insert-timestamp
endif

.PHONY: all x86 arm64 clean arm64-skip banner check-efi-cc check-arm64 FORCE

all: x86
ifeq ($(ARM64_HOST_OK),1)
all: arm64 bootkit-auto.img
else
all: arm64-skip
endif

x86: banner check-efi-cc bootkit.img

arm64: banner check-arm64 bootkit-arm64.img

arm64-skip:
	@echo "== arm64 skipped: needs clang + lld-link (AArch64 PE loader) and $(ARM_CC)" >&2

clean:
	rm -f $(OUT) ptrace_init-arm bootkit.efi bootkit-arm64.efi \
	    bootkit.img bootkit-arm64.img bootkit-auto.img \
	    initrd.cpio initrd-arm.cpio \
	    bootkit/preload.so bootkit/preload-arm.so \
	    bootkit/preload_so.h bootkit/preload_so_arm.h \
	    bootloader/initrd_so.h bootloader/initrd_so_arm.h
	rm -rf $(BUILD)

# The stamps record what the artifacts were built with; they are rewritten
# only when the content changes (or this Makefile is newer, i.e. its build
# rules changed), so unchanged settings leave the artifacts alone and changed
# ones force a rebuild.
$(BUILD):
	@mkdir -p $@

$(FLAGS): FORCE | $(BUILD)
	@printf '%s\n' 'DEFS=$(DEFS)' 'EFI_CC=$(EFI_CC)' 'ARM_CC=$(ARM_CC)' > $@.tmp
	@if cmp -s $@.tmp $@ && [ ! '$(MAKEFILE)' -nt $@ ]; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

# the stick images depend on USB_MB; a separate stamp keeps a size change from
# relinking the loaders
$(IMGFLAGS): FORCE | $(BUILD)
	@printf '%s\n' 'USB_MB=$(USB_MB)' > $@.tmp
	@if cmp -s $@.tmp $@ && [ ! '$(MAKEFILE)' -nt $@ ]; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

banner:
	@echo "== build: DEBUG=$(DEBUG) (1 = serial console + verbose tracer/probe logs)"
	@echo "== probe keys: custom $(if $(CUSTOM_LICENSE_PUBLIC_KEY),$(CUSTOM_LICENSE_PUBLIC_KEY),<built-in>), stock $(if $(MIKRO_LICENSE_PUBLIC_KEY),$(MIKRO_LICENSE_PUBLIC_KEY),<built-in>)"

check-efi-cc:
	@[ -n '$(EFI_CC)' ] || { \
	    echo "ERROR: no EFI compiler found (need clang + lld-link, or" >&2; \
	    echo "       x86_64-w64-mingw32-gcc; override with EFI_CC=...)." >&2; \
	    exit 1; }
	@case '$(EFI_CC)' in \
	    *clang*) ;; \
	    *) '$(EFI_CC)' -dumpmachine 2>/dev/null | grep -q mingw || { \
	           echo "ERROR: $(EFI_CC) does not target PE/Windows; use clang" >&2; \
	           echo "       (--target=x86_64-unknown-windows) or" >&2; \
	           echo "       x86_64-w64-mingw32-gcc." >&2; \
	           exit 1; } ;; \
	esac

check-arm64:
	@command -v clang >/dev/null 2>&1 && command -v lld-link >/dev/null 2>&1 || { \
	    echo "ERROR: arm64 needs clang + lld-link (AArch64 PE loader)" >&2; \
	    exit 1; }
	@command -v '$(ARM_CC)' >/dev/null 2>&1 || { \
	    echo "ERROR: arm64 needs $(ARM_CC) (Debian/Ubuntu: gcc-arm-linux-gnueabihf;" >&2; \
	    echo "       ARM_CC= overrides)" >&2; \
	    exit 1; }

# ---------------------------------------------------------------- x86/x86_64

# The toolchain is rebuilt when tools/musl_i386.sh changed (or the toolchain
# is missing), keyed on the script content rather than its mtime: CI restores
# .toolchain from actions/cache with the mtimes of the run that saved it,
# while the checked-out script always looks newer, which would otherwise
# rebuild musl on every run.
$(I386_CC): FORCE
	@sha=$$(sha256sum '$(TOOLS)/musl_i386.sh' | cut -d' ' -f1); \
	if [ -x '$@' ] && [ "$$(cat '$(TOOLCHAIN_I386)/build.key' 2>/dev/null)" = "$$sha" ]; then exit 0; fi; \
	echo "== i486 musl toolchain missing or tools/musl_i386.sh changed, running it"; \
	'$(TOOLS)/musl_i386.sh'; \
	printf '%s\n' "$$sha" > '$(TOOLCHAIN_I386)/build.key'

# 1. the LD_PRELOAD probe (preload.c): ordinary C built without libc, so its
#    imports (open/write/readlink/getpid/snprintf/...) are resolved at load
#    time by the dynamic linker against the process's libc.  The probe carries
#    the embedded keygen (keygen.c, included by preload.c) and patches the
#    licence public key in mode/keyman.
bootkit/preload.so: bootkit/preload.c bootkit/keygen.c $(FLAGS) $(I386_CC)
	@echo "== i386 compiler: $(I386_CC)"
	$(I386_CC) -shared -fPIC -nostdlib -Os -fno-stack-protector -fvisibility=hidden \
	    -ffunction-sections -fdata-sections -Wl,--gc-sections \
	    $(DEFS) -o $@ bootkit/preload.c
	@echo "built: $@ ($$(wc -c < $@) bytes)"

# 2. embed preload.so into the init binary
bootkit/preload_so.h: bootkit/preload.so $(TOOLS)/embed.py
	$(TOOLS)/embed.py $< $@ preload_so

# 3. ptrace_init itself
$(OUT): bootkit/ptrace_init.c bootkit/preload_so.h $(FLAGS) $(I386_CC)
	$(I386_CC) -static -Os -Wall $(DEFS) -o $@ bootkit/ptrace_init.c
	@echo "built: $@ ($$(wc -c < $@) bytes), bootkit/preload.so ($$(wc -c < bootkit/preload.so) bytes)"

# 4. the initramfs (the just-built ptrace_init alone, cpio/newc) and its
#    embedded C array for the EFI loader
initrd.cpio: $(OUT) $(TOOLS)/mkinitrd.sh
	$(TOOLS)/mkinitrd.sh $< $@

bootloader/initrd_so.h: initrd.cpio $(TOOLS)/embed.py
	$(TOOLS)/embed.py $< $@ initrd_cpio

# 5. the EFI loader (installed as \EFI\BOOT\BOOTKIT.EFI; replaces the EFI
#    shell + startup.nsh trick).  Freestanding x86_64 PE32+ with the initramfs
#    embedded (initrd_so.h, step 4), no libc and no gnu-efi.  Prefer clang +
#    lld-link (--target=x86_64-unknown-windows); a gcc that targets PE, e.g.
#    x86_64-w64-mingw32-gcc, works equally well (EFI_CC=<compiler> forces one).
bootkit.efi: $(X86_EFI_SRCS) $(LOADER_HDRS) bootloader/initrd_so.h $(FLAGS) | check-efi-cc
ifneq ($(findstring clang,$(EFI_CC)),)
	$(EFI_CC) $(X86_EFI_CCFLAGS) $(DEFS) -o $@ $(X86_EFI_SRCS)
else
	@[ -n '$(EFI_CC)' ] || { \
	    echo "ERROR: no EFI compiler found (need clang + lld-link, or" >&2; \
	    echo "       x86_64-w64-mingw32-gcc; override with EFI_CC=...)." >&2; \
	    exit 1; }
	@set -e; obj=$$(mktemp -d); trap 'rm -rf "$$obj"' EXIT; \
	for s in $(X86_EFI_NAMES); do \
	    $(EFI_CC) -c $(X86_EFI_CCFLAGS) $(DEFS) -I bootloader \
	        -o "$$obj/$$s.o" "bootloader/$$s.c"; \
	done; \
	$(EFI_CC) $(X86_EFI_LDFLAGS) -o $@ "$$obj"/*.o
endif
	@echo "built: $@ ($$(wc -c < $@) bytes, via $(EFI_CC))"

# 6. bootkit.img: the x86 installer/removable USB stick, a 32 MB MBR disk
#    with one EFI system partition (FAT, type 0xEF) holding
#    \EFI\BOOT\BOOTX64.EFI.  Flash it (dd if=bootkit.img of=/dev/sdX bs=4M
#    conv=fsync); with no \BOOTKIT.CFG on it the loader starts its
#    direct/removable install menu.
bootkit.img: bootkit.efi $(TOOLS)/bootkit_img.sh $(TOOLS)/mbr.py $(IMGFLAGS)
	USB_MB=$(USB_MB) $(TOOLS)/bootkit_img.sh $@ BKINSTALL 0x4d544b42 bootkit.efi EFI/BOOT/BOOTX64.EFI

# --------------------------------------------------------------- arm64 (CHR)

# As above; ARM_CC is part of the key because the installed musl-gcc wrapper
# execs the compiler recorded when the toolchain was built.
$(ARM_MUSL_CC): FORCE
	@sha=$$(sha256sum '$(TOOLS)/musl_arm.sh' | cut -d' ' -f1); sig="$$sha ARM_CC=$(ARM_CC)"; \
	if [ -x '$@' ] && [ "$$(cat '$(TOOLCHAIN_ARM)/build.key' 2>/dev/null)" = "$$sig" ]; then exit 0; fi; \
	echo "== ARM musl toolchain missing or changed, running tools/musl_arm.sh"; \
	ARM_CC='$(ARM_CC)' '$(TOOLS)/musl_arm.sh'; \
	printf '%s\n' "$$sig" > '$(TOOLCHAIN_ARM)/build.key'

# 1. the ARM32 probe: soft-float, no libc, no DT_NEEDED (same scheme as the
#    i386 one; the RouterOS arm32 libc is soft-float too).  The musl headers
#    come from the arm toolchain built above.
bootkit/preload-arm.so: bootkit/preload.c bootkit/keygen.c $(FLAGS) $(ARM_MUSL_CC)
	@echo "== arm32 compiler: $(ARM_CC) (probe soft-float, sysroot $(TOOLCHAIN_ARM))"
	$(ARM_CC) -mfloat-abi=soft -march=armv7-a -nostdinc -isystem $(ARM_INC) \
	    -shared -fPIC -nostdlib -Os -fno-stack-protector -fvisibility=hidden \
	    -ffunction-sections -fdata-sections -Wl,--gc-sections \
	    $(DEFS) -o $@ bootkit/preload.c
	@echo "built: $@ ($$(wc -c < $@) bytes)"

# 2. embed it into the ARM init binary
bootkit/preload_so_arm.h: bootkit/preload-arm.so $(TOOLS)/embed.py
	$(TOOLS)/embed.py $< $@ preload_so

# 3. the ARM init itself (static musl)
ptrace_init-arm: bootkit/ptrace_init.c bootkit/preload_so_arm.h $(FLAGS) $(ARM_MUSL_CC)
	$(ARM_MUSL_CC) -static -Os -Wall $(DEFS) \
	    -DPRELOAD_SO_HEADER='"preload_so_arm.h"' \
	    -o $@ bootkit/ptrace_init.c
	@echo "built: $@ ($$(wc -c < $@) bytes), bootkit/preload-arm.so ($$(wc -c < bootkit/preload-arm.so) bytes)"

# 4. its initramfs (embedded in the AArch64 loader)
initrd-arm.cpio: ptrace_init-arm $(TOOLS)/mkinitrd.sh
	$(TOOLS)/mkinitrd.sh $< $@

bootloader/initrd_so_arm.h: initrd-arm.cpio $(TOOLS)/embed.py
	$(TOOLS)/embed.py $< $@ initrd_cpio

# 5. the AArch64 EFI loader: freestanding PE32+ (clang + lld-link), the arm32
#    initramfs embedded.  boot_arm64.c boots the stock kernel at
#    \EFI\BOOT\BOOTAA64.EFI through LoadImage/StartImage and serves the
#    initramfs to the kernel's EFI stub as a file on a RAM-backed volume (the
#    5.6 arm64 stub supports "initrd=" from the kernel's own volume; it has no
#    LoadFile2 initrd support).
bootkit-arm64.efi: $(ARM64_EFI_SRCS) $(LOADER_HDRS) bootloader/initrd_so_arm.h $(FLAGS) | check-arm64
	clang --target=aarch64-unknown-windows -ffreestanding -fno-stack-protector \
	    -fshort-wchar -Os -Wall -Wextra -nostdlib $(DEFS) \
	    -DINITRD_SO_HEADER='"initrd_so_arm.h"' \
	    -fuse-ld=lld-link \
	    -Wl,/subsystem:efi_application,/entry:efi_main,/nodefaultlib \
	    -o $@ $(ARM64_EFI_SRCS)
	@echo "built: $@ ($$(wc -c < $@) bytes, via clang --target=aarch64-unknown-windows)"

# 6. bootkit-arm64.img: the arm64 installer/removable USB stick, the same
#    MBR+FAT layout with \EFI\BOOT\BOOTAA64.EFI (the AArch64 removable-media
#    path) holding the loader.
bootkit-arm64.img: bootkit-arm64.efi $(TOOLS)/bootkit_img.sh $(TOOLS)/mbr.py $(IMGFLAGS)
	USB_MB=$(USB_MB) $(TOOLS)/bootkit_img.sh $@ BKINSTALL64 0x4d544b43 bootkit-arm64.efi EFI/BOOT/BOOTAA64.EFI

# ------------------------------------------------------------ both (auto)

# bootkit-auto.img: the both-arch stick - \EFI\BOOT\BOOTX64.EFI and
# \EFI\BOOT\BOOTAA64.EFI plus \BOOTKIT.CFG with "target=auto"
# (bootloader/auto.cfg).  The firmware picks the loader for its own
# architecture and that loader scans the partitions and boots the first
# RouterOS kernel it finds; nothing is installed and nothing on the router is
# touched (no config, no boot entry).
bootkit-auto.img: bootkit.efi bootkit-arm64.efi bootloader/auto.cfg \
		$(TOOLS)/bootkit_img.sh $(TOOLS)/mbr.py $(IMGFLAGS) | check-efi-cc check-arm64
	USB_MB=$(USB_MB) $(TOOLS)/bootkit_img.sh $@ BKAUTO 0x4d544b44 \
	    bootkit.efi EFI/BOOT/BOOTX64.EFI \
	    bootkit-arm64.efi EFI/BOOT/BOOTAA64.EFI \
	    bootloader/auto.cfg BOOTKIT.CFG

FORCE:
