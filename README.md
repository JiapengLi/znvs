# ZNVS

[简体中文](README.zh.md)

A small, standalone C99 key-value store for NOR Flash. No OS, heap, or POSIX dependencies; add [znvs.c](znvs/znvs.c) and [znvs.h](znvs/znvs.h) to your project.

Designed for configuration storage, typically up to 8 keys; also tested with 32 and 64 keys. A partition is split into two equal banks: writes append to the active bank, and garbage collection (GC) copies live values to the other bank before publishing it.

- CRC32 protects metadata and payloads; a separate commit unit completes each record.
- Interrupted writes preserve the committed prefix. An incomplete tail seals the bank; the next mutating write performs GC.
- Banks alternate erases. Every GC copies all live values; there is no hot/cold separation or bad-block replacement.
- No persistent lookup cache. Lookup scans records; GC has worst-case O(N²) scanning cost, where N is the accumulated record count. Operations are synchronous.

## Quick start

Provide these partition-relative byte-address callbacks, then initialize once:

```c
#include "znvs/znvs.h"

int znvs_port_read(void *arg, uint32_t addr, void *buf, size_t len);
int znvs_port_write(void *arg, uint32_t addr, const void *buf, size_t len);
int znvs_port_erase(void *arg, uint32_t addr, size_t len);

static znvs_t store;

int znvs_store_init(void *flash_context)
{
    const znvs_cfg_t cfg = {
        .read = znvs_port_read,
        .write = znvs_port_write,
        .erase = znvs_port_erase,
        .size = 8192,
        .erase_size = 4096,
        .write_size = 4
    };
    return znvs_init(&store, &cfg, flash_context);
}

int znvs_save_flags(uint32_t flags)
{
    return znvs_write(&store, 1, &flags, sizeof(flags));
}
```

Call `znvs_store_init` successfully before using `znvs_save_flags` or `znvs_read`. The instance owns a complete configuration copy, so local `cfg` is safe; the driver context passed as `arg` must remain valid.

IDs are `0..65534`; zero-length writes delete the key. Reads return whole values without silent truncation. A size query (`data=NULL, capacity=0, length!=NULL`) skips payload CRC verification. See [the header](znvs/znvs.h) for the complete API and [the example](examples/basic.c) for a runnable port.

## Port requirements

NOR must erase to `0xff` and program only 1→0. Erase units are uniform powers of two; supported program units are 1, 2, 4, 8, 16, and 32 bytes. Each bank must contain whole erase/program units and have room for its header and records.

Callbacks must complete the entire operation synchronously before returning 0; any nonzero return becomes `ZNVS_EIO`. Reads allow arbitrary addresses and lengths. Write addresses and lengths are aligned, but source pointers may be unaligned. The port handles page splitting, busy polling, timeouts, cache/DMA coherency, and physical partition bounds.

Serialize all operations, including reads; allow one active writer per partition, with no callback re-entry or ISR use. Remount after another instance or a bootloader changes the partition. Values are raw bytes: serialize portable data explicitly and combine fields into one record when atomic updates are required. There are no multi-key transactions.

## Builds

All profiles share the header, instance layout, and media format. CRC32 is always enabled; applications need no feature macros.

| Profile | Included behavior |
| --- | --- |
| `full` (default) | All APIs; initializes blank media and skips identical-value writes. |
| `boot` | init, mount, read, write, delete, max_size; nonempty writes always append. |
| `readonly` | init, mount, read, max_size; write/erase callbacks may be NULL. |

Use `make`, `make PROFILE=boot`, or `make PROFILE=readonly`. CMake accepts `-DZNVS_PROFILE=boot` or `readonly`. When compiling manually, define `ZNVS_PROFILE=1` (boot) or `=2` (readonly) **only for znvs.c**. Boot and readonly require a partition already initialized by full or a prepared production image.

## Failures and recovery

- `ZNVS_ENOSPC`: insufficient capacity; writes perform no programming or erasing.
- `ZNVS_EIO` or corrupt committed metadata invalidates the instance. Restore the device, then call `znvs_mount`; a failed write **may already have committed**, so do not assume rollback.
- `ZNVS_ECORRUPT`: CRC/metadata validation failed. Payload CRC failures allow replacement with a known-good value; remounting does not repair permanent corruption. Do not consume read output after any error.
- GC checks copied payloads before publishing the new bank. Old data remains until the following GC. `znvs_format` destroys the partition and is **not atomic across power failure**; an interrupted format requires an explicit retry.

Recovery is tested with partially completed program/erase operations. CRC detects corruption but provides no correction or authentication; an older snapshot may be selected if publication headers become invalid. CRC does not guarantee rollback prevention under arbitrary corruption. Validate actual power-loss behavior on the target device.

## Footprint and validation

Cortex-M3 Thumb, Clang 22.1.8, `-Oz -flto`, runtime geometry; retained APIs are init/read plus write/delete for writable profiles:

| Profile | Linked ROM | Instance RAM | Local call-chain stack estimate |
| --- | ---: | ---: | ---: |
| full | 2622 B | 52 B | write ≤352 B |
| boot | 2136 B | 52 B | write ≤272 B |
| readonly | 972 B | 52 B | read ≤144 B |

ROM includes C helpers and ARM unwind tables, excluding the driver, startup, and application. RAM includes the configuration copy; stack excludes driver/library/interrupt overhead. Other targets and retained APIs need remeasurement. With 2×4096 B banks and 4 B programming, a 4 B value occupies 20 B and the maximum single payload is 4048 B.

Run `make test`, `make matrix`, `make sanitize`, and `make measure`. Fault injection, randomized models, profile interoperability, performance counters, and measurement conditions are recorded in [tests/RESULTS.txt](tests/RESULTS.txt). Host tests do not replace hardware validation.

Apache-2.0; see [LICENSE](LICENSE) and [NOTICE](NOTICE). Zephyr reference sources under `tests/upstream/` are test-only.
