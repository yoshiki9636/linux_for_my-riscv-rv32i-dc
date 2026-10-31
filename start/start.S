.section .text.start
.global _start
_start:
    li    a0, 0
    lui   a1, 0x801         # a1 = DTB (DTB_ADDR in make_image_reloc.py)
    //li    t0, 0x5c
    lui   t0, 0x800
    jalr  zero, t0, 0

.section .text
.global main_code

main_code:
    /* FPGA diagnostic (2026-07-07): capture ra/sp exactly as they were on
     * entry to CODE, before anything below overwrites them, and print
     * them (marker '0'). STUB (address 0) does not touch ra/sp/tp at all
     * (only a0/a1/t0) - so on a genuine cold boot, whatever the CPU's true
     * reset leaves in ra/sp will show here for the first time this
     * project has looked. If instead this is a wild jalr through a NULL
     * or corrupted function pointer that happens to equal 0 mid-boot
     * (which lands here, since STUB is live, valid code rather than
     * unmapped memory - unlike almost everywhere else on this board),
     * ra/sp will hold real, live, in-progress kernel values instead:
     * ra = the caller's return address (identifies the culprit directly),
     * sp = a live kernel stack pointer (proves this isn't a fresh reset).
     * See project memory - the user reported "AB" appearing mid-boot with
     * no panic/oops message, i.e. not a real hardware reset. */
    mv    s1, ra
    mv    s2, sp
    /* FPGA (2026-10-29): this stack used to be 0x0027C000 - inside the live
     * kernel .text since the kernel links at 0. send_hex32's
     * `sw ra,12(sp); sw s3,8(sp)` wrote 0x0027bffc/0x0027bff8 on every boot,
     * and with the #869 layout that clobbered memalloc_flags_save+0x8 with
     * 0x00000001 (s3 of the last stack_dump_loop pass) -> illegal
     * instruction in loop_init. Use the top of this CODE window instead
     * (code is ~340 bytes at 0x00800000; the DTB starts at 0x00801000 and
     * the stack grows down, away from it). */
    lui   sp, 0x801
    lui   tp, 0x801
    addi  tp, tp, -0x400

    li    t5, 0x30
    jal   ra, send_byte
    mv    a0, s1
    jal   ra, send_hex32
    li    t5, 0x2f
    jal   ra, send_byte
    mv    a0, s2
    jal   ra, send_hex32
    li    t5, 0x3a
    jal   ra, send_byte

    li    t5, 0x41
    jal   ra, send_byte
    li    a0, 0
    lui   a1, 0x801         # a1 = DTB (DTB_ADDR in make_image_reloc.py)
    li    t5, 0x42
    jal   ra, send_byte

    /* FPGA diagnostic (2026-07-11): when `ra` alone isn't informative (e.g.
     * exactly 0, as seen on one wild-jump-to-NULL capture where the ra
     * register gave no clue), dump a few words starting at the captured
     * sp (s2) too. If this is a genuinely live, in-progress kernel stack
     * (not a fresh reset - the whole point of this diagnostic, see the
     * comment above main_code), a non-leaf interrupted function will
     * typically have already saved its own `ra` a few words up from sp in
     * its prologue, so it can still identify the real caller even when the
     * live ra register can't. s2 is only read here (plain loads, no
     * writes) so this is safe regardless of what it actually points at.
     * Print "SD:" then 8 words at s2+0..s2+28, '/'-separated. s3 (loop
     * count) and s4 (scan pointer) are both safe across send_hex32/
     * send_byte: send_hex32 saves/restores s3 itself, and neither routine
     * touches s4 at all. */
    li    t5, 0x53
    jal   ra, send_byte
    li    t5, 0x44
    jal   ra, send_byte
    li    t5, 0x3a
    jal   ra, send_byte
    li    s3, 8
    mv    s4, s2
.stack_dump_loop:
    lw    a0, 0(s4)
    jal   ra, send_hex32
    li    t5, 0x2f
    jal   ra, send_byte
    addi  s4, s4, 4
    addi  s3, s3, -1
    bnez  s3, .stack_dump_loop
    li    t5, 0x3a
    jal   ra, send_byte

    li    t0, 0x5c
    jalr  zero, t0, 0

/* send_hex32: print 8 ASCII hex digits of a0 via UART (calls send_byte).
 * Preserves a0. Needs a working stack (sp must already be set up by the
 * caller - not a leaf routine, calls send_byte itself). */
send_hex32:
    addi  sp, sp, -16
    sw    ra, 12(sp)
    sw    s3, 8(sp)
    mv    s3, a0
    li    t2, 8
.hex_loop:
    addi  t2, t2, -1
    slli  t3, t2, 2
    srl   t4, s3, t3
    andi  t4, t4, 0xF
    li    t5, 10
    blt   t4, t5, .hex_digit
    addi  t5, t4, 87
    j     .hex_send
.hex_digit:
    addi  t5, t4, 48
.hex_send:
    jal   ra, send_byte
    bnez  t2, .hex_loop
    lw    s3, 8(sp)
    lw    ra, 12(sp)
    addi  sp, sp, 16
    ret

send_byte:
    li    t0, 0xC000FC00
wait:
    lw    t1, 4(t0)
    andi  t1, t1, 1
    bnez  t1, wait
    andi  t5, t5, 0xFF
    sw    t5, 0(t0)
    ret
