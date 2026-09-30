/*
 * Standalone interface for Zephyr NVS.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZNVS_H
#define ZNVS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Define consistently for the library AND every application translation unit. */
#ifndef ZNVS_IO_SIZE
#define ZNVS_IO_SIZE 32
#endif
#ifndef ZNVS_CACHE_SIZE
#define ZNVS_CACHE_SIZE 0
#endif
#ifndef ZNVS_DATA_CRC
#define ZNVS_DATA_CRC 1
#endif

#if ZNVS_IO_SIZE < 8 || (ZNVS_IO_SIZE & (ZNVS_IO_SIZE - 1)) || ZNVS_IO_SIZE > 256
#error "ZNVS_IO_SIZE must be a power of two in [8, 256]"
#endif
#if ZNVS_CACHE_SIZE < 0 || (ZNVS_CACHE_SIZE & (ZNVS_CACHE_SIZE - 1)) || ZNVS_CACHE_SIZE > 32768
#error "ZNVS_CACHE_SIZE must be zero or a power of two, at most 32768"
#endif
#if ZNVS_DATA_CRC != 0 && ZNVS_DATA_CRC != 1
#error "ZNVS_DATA_CRC must be 0 or 1"
#endif

/* Values -2/-3/-4 follow zat's EINVAL/ENOSPC/EIO convention; no errno dependency. */
enum {
    ZNVS_OK       = 0,
    ZNVS_ERROR    = -1,
    ZNVS_EINVAL   = -2,
    ZNVS_ENOSPC   = -3,
    ZNVS_EIO      = -4,
    ZNVS_ENOENT   = -5,
    ZNVS_ESTATE   = -6,
    ZNVS_ECORRUPT = -7
};

#define ZNVS_ID_MAX UINT16_C(65534)

typedef struct znvs znvs_t;

/* Partition-relative byte offsets. Complete the WHOLE operation synchronously.
 * Return ZNVS_OK only after completion; any nonzero result becomes ZNVS_EIO.
 * read: arbitrary offset/length. write: aligned offset/length, but the source
 * pointer can be unaligned. erase: erase_size-aligned offset/length.
 * NOR semantics: erased bytes 0xff; programming only changes 1 to 0.
 * Port must handle program-page splitting, busy polling, cache/DMA coherency,
 * source-pointer alignment requirements and bounds of the physical partition.
 */
typedef int (*znvs_read_fn)(void *arg, uint32_t off, void *buf, size_t len);
typedef int (*znvs_write_fn)(void *arg, uint32_t off, const void *buf, size_t len);
typedef int (*znvs_erase_fn)(void *arg, uint32_t off, size_t len);

typedef struct {
    znvs_read_fn read;
    znvs_write_fn write;
    znvs_erase_fn erase;
    uint32_t size;          /* Partition bytes; whole sectors, 2..65535 sectors. */
    uint32_t sector_size;   /* Power of two, <=65536; multiple of erase_size. */
    uint32_t erase_size;    /* Uniform physical erase unit, power of two. */
    uint16_t write_size;    /* Power of two, <=ZNVS_IO_SIZE. */
} znvs_cfg_t;

/* Caller-owned, no heap. Fields are private state despite being exposed for
 * static allocation. cfg must remain alive AND immutable (prefer static const).
 * All operations on an instance, including reads, require external serialization.
 * Callbacks must not re-enter it. Not an ISR API. Separate partitions may use
 * separate instances; serialize shared flash hardware in the port as needed.
 */
struct znvs {
    const znvs_cfg_t *cfg;
    void *arg;
    uint32_t ate_wra;
    uint32_t data_wra;
    uint16_t sector_count;
    uint8_t ready;
#if ZNVS_CACHE_SIZE > 0
    uint32_t lookup_cache[ZNVS_CACHE_SIZE];
#endif
};

/* Configure and mount. No need to pre-zero *fs. Does NOT erase on a mount error.
 * Mount/recovery can write metadata and erase sectors belonging to an interrupted
 * GC. Use only on a dedicated, correctly configured NVS partition.
 * On a mount error with valid cfg, znvs_format() remains available explicitly.
 */
int znvs_init(znvs_t *fs, const znvs_cfg_t *cfg, void *arg);
int znvs_mount(znvs_t *fs);

/* DESTRUCTIVE: erase the entire configured partition, then mount it.
 * Not atomic across power failure; retry format after an interrupted format.
 */
int znvs_format(znvs_t *fs);

/* IDs 0..65534; 65535 is reserved. len==0 deletes (no distinct empty blob).
 * Returns OK both for a new commit and for an identical-value no-op.
 * A nonzero I/O callback result invalidates the instance: restore the device,
 * then mount again before ANY normal operation. A failed operation may have
 * committed; after mount it may expose the old or new value, never assume rollback.
 */
int znvs_write(znvs_t *fs, uint16_t id, const void *data, size_t len);
int znvs_delete(znvs_t *fs, uint16_t id);

/* Full-value reads, not silent truncation. length is optional for normal reads.
 * Query size: data=NULL, capacity=0, length!=NULL -> OK + size, NO data CRC check.
 * Undersized buffer: ENOSPC + required length, buffer unchanged.
 * On read/CRC error buffer contents are unspecified; consume only on OK.
 * Output length is 0 when not found, or the found length once known.
 */
int znvs_read(znvs_t *fs, uint16_t id, void *data, size_t capacity, size_t *length);

/* history=0 is latest, 1 is previous, etc. Deletion records count as a version
 * and return ENOENT. GC may discard history: this is NOT an audit log.
 */
int znvs_read_hist(znvs_t *fs, uint16_t id, uint16_t history,
                   void *data, size_t capacity, size_t *length);

/* O(1): configured maximum payload, and current-sector payload that fits without
 * rotation/GC. Return 0 if unconfigured (max_size) or unmounted (available).
 * available is NOT total free space; a larger write can succeed after automatic GC.
 */
size_t znvs_max_size(const znvs_t *fs);
size_t znvs_available(const znvs_t *fs);

/* Explicitly rotate and collect. Usually unnecessary; extra calls wear flash.
 * Useful to move erase latency away from a time-sensitive write. Still blocking.
 */
int znvs_rotate(znvs_t *fs);
void *znvs_arg(const znvs_t *fs);

#ifdef __cplusplus
}
#endif
#endif
