/*
 * 1943: the flash store, for boards without PSRAM; see c1943_store.c.
 */
#ifndef C1943_STORE_H
#define C1943_STORE_H

#include <stdint.h>

#include "c1943.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    const uint8_t *base; /* where the store is read (XIP) */
    uint32_t offset;     /* its offset in flash, for erase and program */
    uint32_t size;
    /* flash offsets; erase in 64 KB blocks, program in 256-byte pages */
    void (*erase)(uint32_t offset, uint32_t len);
    void (*program)(uint32_t offset, const uint8_t *data, uint32_t len);
    /* called before a set is copied, and about every 64 KB of it; may be
     * NULL */
    void (*copying)(int set);
} c1943_flash_t;

/* The buffer c1943_store_copy needs: two 32 KB ROMs and a 4 KB sector. */
#define C1943_STORE_TEMP (0x10000 + 0x1000)

/* Takes the store into use. Reads only. */
void c1943_store_open(const c1943_flash_t *flash);

/* True when the store is open and holds a set. */
int c1943_store_room(void);

/* The bytes a set takes, and the sets the store holds at once (0, 1 or 2). */
uint32_t c1943_store_slot_size(void);
int c1943_store_slots(void);

/* The variant of the set in the store, its data checked; -1 when it is not
 * there. */
int c1943_store_variant(c1943_set_id_t set);

/* The set's parts in the store; 0 when it is not there. */
int c1943_store_parts(c1943_set_id_t set, const uint8_t **parts);

/* Builds the set's variant v from dir in the store, replacing what its slot
 * held, through temp (C1943_STORE_TEMP bytes). Returns the mask of the files
 * found; the copy worked when c1943_store_variant(set) == v afterwards. */
c1943_files_t c1943_store_copy(c1943_set_id_t set, int v, const c1943_io_t *io, const char *dir, uint8_t *temp,
                               uint32_t temp_size);

#ifdef __cplusplus
}
#endif

#endif
