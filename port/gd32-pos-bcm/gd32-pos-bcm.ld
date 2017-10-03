MEMORY
{
    rom (rx) : ORIGIN = 0x08000000, LENGTH = 508K
    ram (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}

INCLUDE libopencm3_stm32f1.ld
