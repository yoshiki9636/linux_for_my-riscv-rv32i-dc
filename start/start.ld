MEMORY {
    STUB (rx) : ORIGIN = 0x00000000, LENGTH = 16
    CODE (rx) : ORIGIN = 0x00800000, LENGTH = 4096
}

SECTIONS {
    . = 0x00000000;
    .text.start : { *(.text.start) } > STUB

    . = 0x00800000;
    .text : { *(.text*) } > CODE
}

