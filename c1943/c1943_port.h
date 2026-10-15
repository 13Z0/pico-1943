/*
 * Build glue shared by the 1943 core. The core is plain C with no Pico
 * dependencies, so the same sources build for the board and for the host
 * harness in hosttest/. On the board the hot paths go to SRAM.
 *
 * So do the tables they read: C1943_RAM_DATA, the SDK's __not_in_flash (not
 * merely leaving out const: GCC puts a table that is never written in flash
 * all the same). Flash is read through the XIP cache, which on HSTX boards
 * core0 fills with the graphics it draws from PSRAM while core1 makes the
 * sound; a table there misses all the time, and each miss waits for the QSPI
 * bus. The same goes for library calls (memset, memcpy, 64-bit division),
 * which live in flash: the hot paths use loops.
 */
#ifndef C1943_PORT_H
#define C1943_PORT_H

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "pico.h"
#define C1943_HOT(f) __not_in_flash_func(f)
#define C1943_RAM_DATA __not_in_flash("c1943")
#else
#define C1943_HOT(f) f
#define C1943_RAM_DATA
#endif

#endif
