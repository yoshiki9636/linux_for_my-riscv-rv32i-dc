#!/usr/bin/env python3
# make_image_reloc.py - Image.reloc generation for FPGA RISC-V boot
# Addresses are read dynamically from System.map each run.

import struct, subprocess as sp, tempfile, os, sys

# Paths (2026-11-01): this script lives at the top of the repro repo, and the
# kernel tree is always cloned/built in linux/ right under it. Kernel build
# outputs are read from linux/; initramfs.cpio, start/start.elf and the
# Image.reloc/Image.hex outputs live next to this script. All paths are based
# on this script's location, so it works from any current directory.
TOP  = os.path.dirname(os.path.abspath(__file__))
KDIR = os.path.join(TOP, 'linux')
VMLINUX   = os.path.join(KDIR, 'vmlinux')
SYSMAP    = os.path.join(KDIR, 'System.map')
IMAGE     = os.path.join(KDIR, 'arch/riscv/boot/Image')
DTB       = os.path.join(KDIR, 'arch/riscv/boot/dts/myfpga/my_fpga.dtb')
INITRAMFS = os.path.join(TOP, 'initramfs.cpio')
START_ELF = os.path.join(TOP, 'start/start.elf')
OUT_RELOC = os.path.join(TOP, 'Image.reloc')
OUT_HEX   = os.path.join(TOP, 'Image.hex')
for _p in (VMLINUX, SYSMAP, IMAGE, DTB, INITRAMFS, START_ELF):
    if not os.path.isfile(_p):
        sys.exit(f'ERROR: {_p} not found (kernel must be built in {KDIR}, '
                 f'start.elf in {os.path.join(TOP, "start")})')

# FPGA (2026-10-29): while the kernel was CONFIG_RELOCATABLE, vmlinux was
# linked with -shared and an undefined symbol was NOT a link error (the call
# silently went to address 0, #866). Since AR-899 the kernel is
# non-relocatable so the linker catches it, but keep this cheap check.
_undef = sp.run(['riscv32-unknown-linux-gnu-nm', '-u', VMLINUX],
                capture_output=True, text=True, check=True).stdout.split()
_undef = [s for s in _undef if s != 'U']
if _undef:
    raise SystemExit(f'*** vmlinux has undefined symbols: {_undef} ***')

# ── helpers ──────────────────────────────────────────────────────────────────

def load_sysmap(path=SYSMAP):
    syms = {}
    with open(path) as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) >= 3:
                syms[parts[2]] = int(parts[0], 16)
    return syms

def load_sysmap_code_symbols(path=SYSMAP):
    """Sorted (addr, name) list of code-type symbols (t/T/w/W), for the
    overlay collision check below."""
    entries = []
    with open(path) as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) >= 3 and parts[1] in 'tTwW':
                entries.append((int(parts[0], 16), parts[2]))
    entries.sort()
    return entries

def check_overlay_collision(code_syms, region_name, start, length):
    """FPGA safety check (2026-07-07): CODE@0x2ea000 and DTB@0x402000 are
    blindly memcpy'd on top of the live kernel image (CONFIG_RELOCATABLE +
    KERNEL_LINK_ADDR=0 on this NOMMU board means the kernel links starting
    at address 0, so there's no guaranteed-empty space to put these at
    fixed addresses - see CLAUDE.md). Any kernel/config change can relink a
    *different*, possibly load-bearing function into these hardcoded
    windows. This bit real: on 2026-07-07, adding ~68 bytes of .bss
    (fs/super.c's fpga_freed_sb_ring[]) shifted layout just enough that
    riscv_intc_domain_alloc() - genuinely called on every boot right after
    "riscv-intc: N local interrupts mapped" - moved into the CODE window,
    causing a reproducible (3/3) crash that cost several real-hardware
    test cycles to root-cause. This check makes that loud at build time
    instead of silent on hardware. It cannot tell "harmless because
    unreachable on this DTS" (e.g. dw_mci_*/mmc, unused on this board) from
    "will crash the next boot" (e.g. riscv_intc_domain_alloc) - a human
    still has to judge each hit - but it turns a multi-cycle real-hardware
    debugging session into an instant, visible warning to check first."""
    end = start + length
    hits = []
    prev = None
    for addr, name in code_syms:
        if addr < start:
            prev = (addr, name)
            continue
        if addr < end:
            hits.append((addr, name))
        else:
            break
    # Only flag the preceding symbol if it's within 4KB of the window - most
    # functions are far smaller than that, so anything further back is
    # essentially guaranteed not to actually extend into the window (avoids
    # flagging e.g. a linker boundary symbol megabytes away every run).
    if prev and start - prev[0] < 0x1000:
        hits.insert(0, (prev[0], prev[1] + ' (starts before window, may extend into it - check its size)'))
    if hits:
        print(f'*** WARNING: {region_name} overlay [{start:#010x}-{end:#010x}) overlaps live code symbols: ***')
        for addr, name in hits:
            print(f'    {addr:#010x} {name}')
        print(f'*** Check System.map / objdump whether each is reachable on this DTS before trusting this build. ***')
    return hits

def write_u32(buf, addr, val):
    struct.pack_into('<I', buf, addr, val & 0xFFFFFFFF)

def read_u32(buf, addr):
    return struct.unpack_from('<I', buf, addr)[0]

def extend_to(buf, size):
    """bytearrayをsizeまでゼロ埋めで拡張する"""
    if len(buf) < size:
        buf.extend(b'\x00' * (size - len(buf)))

# ── load Image ───────────────────────────────────────────────────────────────

with open(IMAGE, 'rb') as f:
    mem = bytearray(f.read())

syms = load_sysmap()
code_syms = load_sysmap_code_symbols()

# ── 1. RELA ──────────────────────────────────────────────────────────────────
# Since AR-899 (2026-10-30) the kernel is built with CONFIG_RELOCATABLE=n and
# CONFIG_PHYS_RAM_BASE=0x0: it is linked at 0 with absolute addresses already
# resolved, so there is nothing to apply. Fail if a relocatable kernel shows
# up again, because nothing on the board would apply its relocations.
if syms.get('__rela_dyn_end', 0) != syms.get('__rela_dyn_start', 0):
    sys.exit('ERROR: kernel has .rela.dyn (CONFIG_RELOCATABLE=y?) - '
             'build it with CONFIG_RELOCATABLE=n, CONFIG_PHYS_RAM_BASE=0x0')

# ── 2-11. instruction/function binary patches: all removed ──────────────────
# mhartid→li a0,0 and wfi→nop (disabled 2026-10-28, the RTL handles both),
# ebreak→nop (removed 2026-07-23), sc.w retry-branch→nop (disabled when the
# A extension was fixed in RTL), and the 2026-07 function→ret stubs. The
# kernel binary is used exactly as linked. See git history of this file
# before 2026-10-30 if any of them is ever needed again.

# ── 12. DTB_ADDR ─────────────────────────────────────────────────────────────
# FPGA (2026-07-07): DTB_ADDR relocated from 0x00402000 to 0x00801000 -
# 0x402000 sat 8 bytes into the tail of queue_work.isra.0 (a genuinely
# load-bearing function; see check_overlay_collision() above), the same
# class of hardcoded-overlay collision CODE_ADDR just got moved to fix (see
# that section's comment). Unlike CODE_ADDR, DTB_ADDR has no separate
# hand-built STUB dependency - every reference to it (this setup_vm patch,
# _dtb_early_va/pa below, and the actual DTB embed) is computed from this
# one Python variable, so relocating it is fully self-contained here. Must
# stay 4KB-aligned (lui only sets the upper 20 bits).

DTB_ADDR = 0x00801000

# FPGA (2026-10-30, AJ-893): start/start.S now passes the DTB address in
# a1 (lui a1, DTB_ADDR>>12), which upstream head.S hands to setup_vm(). The
# old patch overwrote setup_vm's first instruction (its `addi sp,sp,-16`
# prologue) with `lui a0, DTB_ADDR>>12`, so setup_vm ran without a stack
# frame; it and the _dtb_early_va/pa pokes are gone. The STUB check below
# makes sure start.S still agrees with DTB_ADDR.

# ── 14. boot_command_line ────────────────────────────────────────────────────
# Removed 2026-10-30 (AP-897): the poke was inert, because
# early_init_dt_scan_chosen() overwrites boot_command_line with the DTS
# /chosen/bootargs. my_fpga.dts is the only copy of the command line now.

# ── 15. DTB @ DTB_ADDR ───────────────────────────────────────────────────────

with open(DTB, 'rb') as f:
    dtb = f.read()
check_overlay_collision(code_syms, 'DTB', DTB_ADDR, len(dtb))
extend_to(mem, DTB_ADDR + len(dtb))
mem[DTB_ADDR:DTB_ADDR+len(dtb)] = dtb
print(f'DTB: {len(dtb)} bytes @ {DTB_ADDR:#010x}')

# ── 16. initramfs.cpio @ 0x00900000 ─────────────────────────────────────────

INITRD_ADDR = 0x00900000
# Must match linux,initrd-end in my_fpga.dts. 2026-07-16: the DTS used to say
# 0x913000 (sized for a long-gone 74KB cpio) while the real busybox cpio is
# ~978KB - the kernel saw only the first 77KB and /init (the last cpio member)
# was never extracted. Keep the DTS window fixed at 0xA00000 and hard-fail
# here if the cpio ever outgrows it, instead of silently truncating again.
# 2026-10-27: widened to 0xB00000 for the ~1.12MB multi-applet busybox.
INITRD_END_DTS = 0x00B00000
with open(INITRAMFS, 'rb') as f:
    initrd = bytearray(f.read())
if INITRD_ADDR + len(initrd) > INITRD_END_DTS:
    raise SystemExit(
        f'ERROR: initramfs.cpio ends at {INITRD_ADDR + len(initrd):#x}, past the '
        f'DTS linux,initrd-end window {INITRD_END_DTS:#x} - enlarge both together')

# NOMMU binfmt_elf_fdpic only accepts ET_DYN (PIE) executables. The old
# ET_EXEC→ET_DYN header patch was removed 2026-10-30: the only ET_EXEC file
# it ever hit was an unused leftover /init.bin (dropped from the cpio). Link
# userland with -static-pie instead; fail here if an ET_EXEC sneaks in.
def _cpio_elves(buf):
    pad = lambda n: (4 - n % 4) % 4
    pos = 0
    while pos < len(buf):
        if buf[pos:pos+6] != b'070701':
            pos += 1          # zero padding between concatenated archives
            continue
        f = [int(buf[pos+6+8*i:pos+14+8*i], 16) for i in range(13)]
        fsz, nsz = f[6], f[11]
        name = buf[pos+110:pos+110+nsz-1].decode()
        ds = pos + 110 + nsz + pad(110 + nsz)
        if fsz >= 18 and buf[ds:ds+4] == b'\x7fELF':
            yield name, struct.unpack_from('<H', buf, ds + 16)[0]
        pos = ds + fsz + pad(fsz)
_bad = [n for n, t in _cpio_elves(initrd) if t != 3]
if _bad:
    sys.exit(f'ERROR: initramfs has non-PIE (ET_EXEC) ELF files: {_bad}')
print(f'initramfs: {len(initrd)} bytes @ {INITRD_ADDR:#010x}')

extend_to(mem, INITRD_ADDR + len(initrd))
mem[INITRD_ADDR:INITRD_ADDR+len(initrd)] = initrd
# 2026-07-19: zero-fill the rest of the fixed DTS initrd window. The kernel
# unpacks the WHOLE [initrd-start, initrd-end) range; after the cpio TRAILER
# it skips zeros but treats any nonzero residue as a concatenated archive.
# kernel#776 hit exactly that: stale DRAM garbage in the ~69KB tail produced
# "Initramfs unpacking failed: invalid magic at start of compressed archive"
# (files were extracted, but keep the window deterministic and the log clean).
extend_to(mem, INITRD_END_DTS)
print(f'initramfs window zero-padded to {INITRD_END_DTS:#010x}')

# ── 17. STUB @ 0x00000000 ────────────────────────────────────────────────────

with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as tf:
    _tmp = tf.name
try:
    sp.check_call([
        'riscv32-unknown-linux-gnu-objcopy',
        '-O', 'binary', '--only-section=.text.start',
        START_ELF, _tmp
    ])
    with open(_tmp, 'rb') as f:
        stub = f.read()
finally:
    os.unlink(_tmp)
_lui_a1_dtb = ((DTB_ADDR >> 12) << 12) | (11 << 7) | 0x37
if _lui_a1_dtb not in struct.unpack_from(f'<{len(stub)//4}I', stub):
    sys.exit(f'ERROR: start.elf STUB does not set a1 = DTB_ADDR ({DTB_ADDR:#x}); fix start/start.S')
mem[0:len(stub)] = stub
print(f'STUB: {len(stub)} bytes @ 0x00000000')

# ── 18. CODE @ 0x00800000 ────────────────────────────────────────────────────
# FPGA (2026-07-07): relocated from 0x002ea000, which sat inside a densely
# packed cluster of load-bearing IRQ chip callbacks (irq-riscv-intc.c /
# irq-riscv-aplic-main.c) - see check_overlay_collision() above. Any small
# kernel size change could (and on 2026-07-07, did) relink a live function
# into that hardcoded window. 0x00800000 sits ~0.5MB past this build's _end
# (~0x776599) and ~1MB before initramfs (0x900000) - comfortable headroom in
# both directions. start/start.S's STUB (`lui t0, 0x800`) and start/start.ld
# were updated to match; start.elf was rebuilt. (The dead `patch_code`
# entry point was removed from start.S on 2026-10-30.)

with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as tf:
    _tmp = tf.name
try:
    sp.check_call([
        'riscv32-unknown-linux-gnu-objcopy',
        '-O', 'binary', '--only-section=.text',
        START_ELF, _tmp
    ])
    with open(_tmp, 'rb') as f:
        code = f.read()
finally:
    os.unlink(_tmp)
CODE_ADDR = 0x00800000
check_overlay_collision(code_syms, 'CODE', CODE_ADDR, len(code))
# start/start.S jumps to a hardcoded `li t0, <_start_kernel>`; make sure it
# still matches this kernel's System.map (2026-10-30).
_sk = syms['_start_kernel']
_li_t0 = ((_sk & 0xfff) << 20) | (5 << 7) | 0x13       # addi t0, zero, _sk
if _sk >= 0x800 or _li_t0 not in struct.unpack_from(f'<{len(code)//4}I', code):
    sys.exit(f'ERROR: start.elf CODE does not jump to _start_kernel ({_sk:#x}); fix start/start.S')
extend_to(mem, CODE_ADDR + len(code))
mem[CODE_ADDR:CODE_ADDR+len(code)] = code
print(f'CODE: {len(code)} bytes @ {CODE_ADDR:#010x}')

# ── output ───────────────────────────────────────────────────────────────────

with open(OUT_RELOC, 'wb') as f:
    f.write(mem)
print(f'Output: {OUT_RELOC}')

# Generate Image.hex: one 32-bit LE word per line, format " xxxxxxxx\n"
with open(OUT_HEX, 'w') as f:
    for i in range(0, len(mem), 4):
        chunk = bytes(mem[i:i+4]) + b'\x00' * (4 - len(mem[i:i+4]))
        w = struct.unpack_from('<I', chunk, 0)[0]
        f.write(f' {w:08x}\n')
print(f'Output: {OUT_HEX}')
