# profiler

`profiler` is a C99 compiler-instrumented function profiler for embedded and host applications. It
collects call counts and execution-cycle statistics in fixed, statically allocated memory,
then writes the results as binary PROF-BIN, CSV, or a readable terminal table.

## How it works

- GCC's `__cyg_profile_func_enter` / `__cyg_profile_func_exit` hooks support compiler-
  instrumented C functions. Compile the code you want measured with `-finstrument-functions`.
- Metrics are aggregated by function address: call count, total, minimum, and maximum
  cycles. The profiler does not retain a chronological trace.
- All storage is internal and static: up to 128 functions, 16 nested frames per
  context, and 8 execution contexts plus an independent bare-metal stack. There is no heap allocation.
- Probe overhead is calibrated at `profiler_start()` and subtracted from measurements.
  Durations use active CPU cycles: time spent while a context is switched out is excluded
  when the port reports context switches.
- The core uses platform hooks for its timer and critical sections. A weak context hook
  defaults to `NULL`, which selects one shared stack for bare-metal use.

## Build integration

The CMake target is named `profiler`. By default it selects the `custom` platform port
(implemented by the application) and `null` transport. Select a built-in backend before
adding the subdirectory:

```cmake
set(PROFILER_PORT "dwt" CACHE STRING "")
set(PROFILER_TRANSPORT "uart_stm32_hal_dma" CACHE STRING "")
add_subdirectory(path/to/profiler profiler_build)
target_link_libraries(firmware PRIVATE profiler)
```

The project must provide `hashmap` and `stream` either in `profiler/modules/`, as sibling
repositories, or through `HASHMAP_DIR` and `STREAM_DIR`. For an STM32 HAL transport, link
the target that provides the selected HAL headers and symbols:

```cmake
target_link_libraries(profiler PRIVATE stm32cubemx)
```

Available `PROFILER_PORT` values:

| Port | Timer / platform |
|---|---|
| `custom` | Application implementation of `port/profiler_port.h` (default) |
| `dwt` | ARM Cortex-M DWT cycle counter |
| `16bit_it` | 16-bit timer extended in its overflow ISR |
| `2x16bit` | Cascaded 16-bit timers |
| `win` | Windows performance counter |
| `posix` | POSIX monotonic clock |
| `riscv` | RISC-V `mcycle` counter |

Available `PROFILER_TRANSPORT` values are `null` (default), `memory`, `uart_stm32_hal_dma`,
and `uart_stm32_hal_it`. UART backends provide HAL UART callbacks; do not also define the
callbacks owned by the selected backend. DMA UART transmission requires TX and RX DMA
handles on the UART. The interrupt backend receives commands one byte at a time and sends
responses synchronously.

## Application API

```c
#include "profiler.h"

if (!profiler_init(timer_frequency_hz, uart_handle_or_null)) {
    Error_Handler();
}

profiler_start();
/* Run the code being measured. */
(void)profiler_process(); /* Service transport commands in the main loop. */
profiler_stop();

/* Or dump directly to an application-provided Stream. */
(void)profiler_dump(my_stream, PROFILER_DUMP_TOP);
```

`profiler_init()` requires a nonzero timestamp frequency in Hz and initializes the
platform timer. A non-NULL second argument initializes the selected transport; pass NULL
for a headless application. `profiler_start()` enables measurement and calibrates probe
overhead on first use. `profiler_dump()` temporarily pauses a running profiler while it
writes output, clears open stack frames, and then resumes it if it was running. Use
`profiler_reset()` to clear metrics and diagnostics.

Dumping discards open frames in all contexts. In an RTOS, calls that were suspended
across a dump can therefore increment `unmatched_exits` when they return; this diagnostic
is not by itself evidence of stack corruption. Task entries that never return have no
completed-call metric.

`profiler_dump()` accepts `PROFILER_DUMP_BIN`, `PROFILER_DUMP_CSV`, or
`PROFILER_DUMP_TOP`. The binary format is intended for capture and offline analysis; CSV
and TOP are text outputs. Both report function addresses; resolve names offline with
the ELF from the same build.

### Instrumenting your code

For compiler instrumentation, compile application C sources with
`-finstrument-functions`. Do not instrument the profiler library or platform callbacks:
the library disables instrumentation for its own sources, and custom port code should
also be compiled with `-fno-instrument-functions`.

For a GCC CMake target, after adding the profiler subdirectory:

```cmake
profiler_instrument_target(firmware
    EXCLUDE_FILES stm32g0xx_it.c
    EXCLUDE_FUNCTIONS fatal_error)
```

The helper applies C-only instrumentation. GCC exclusion lists match **substrings**,
not globs. Defaults exclude profiler core/port/context/transport, hashmap/stream,
`Drivers`/`CMSIS` paths, system/startup files, `main`, system-clock setup, error handlers and the
timer callback. Inspect your compile commands and extend these lists for every
function reachable from the timer, critical-section, context-ID and transport paths.
The helper never enables instrumentation on another target or prebuilt library.
For Clang use per-source flags and `NO_INST`; GCC's exclusion-list flags are not portable.

Keep interrupt/fault handlers uninstrumented unless your port provides independent ISR
contexts and correct switch notifications. Every hook runs a bounded critical section:
full instrumentation increases interrupt latency, code size and call-stack depth.
GCC also instruments inlined functions; compile-time inlining is not an exclusion.
There is no portable `-finstrument-functions-once` solution for per-call timing.
Assembly and prebuilt uninstrumented libraries produce no events. A normal instrumented
C wrapper can measure assembly, including the wrapper's own call/return work.
Non-returning functions have no exit metric; mismatched exits discard stale frames.

Override `PROFILER_CAPACITY_FUNCTIONS`, `PROFILER_CAPACITY_STACK` and
`PROFILER_CAPACITY_CONTEXTS` when compiling the library if needed. The function capacity
must be a power of two up to 32768; stack/context capacities must be nonzero. Increasing
them consumes static RAM and increases worst-case bookkeeping time. Diagnostics report
capacity exhaustion; overflowing nested calls are ignored until their exits unwind.

Calibration executes the actual non-inlined entry/exit hooks and estimates their
measurement-window and nested costs. It does **not** remove every compiler-generated
argument-setup, prologue/epilogue or cache/bus cost. Measurements describe the instrumented
binary, not an exact reconstruction of its uninstrumented timing. Validate the overhead
and maximum masked-interrupt time on each target/optimization level. Hooks before init
are disabled; `profiler_start()` does not enable an uninitialized engine.
First-start calibration holds a critical section for each single/nested sample, not
for the complete sampling loop. Interrupts and scheduling can run between samples
unless the caller already masked them. Normal measurement hooks remain disabled until
calibration completes; a concurrent start is ignored, and stop/reset cancels the pending
start. Each sample still masks interrupts, so its target-specific maximum needs validation.

### Migrating from scope macros

`PROFILER_SCOPE`, `PROFILER_FUNCTION`, `PROFILER_FUNCTION_NAMED`, `PROFILER_FN_ID`
and the public `profiler_scope_enter/exit` API were removed. Remove the surrounding
scope/function probes, call your workload normally, and enable compiler instrumentation
on its implementation. A remaining `PROFILER_SCOPES_ENABLED` definition is a build error.
There is no manual-measurement replacement API. The PROF-BIN v4 format is unchanged.

### Multiple execution contexts

The default `profiler_port_context_id()` returns NULL, so all hooks share the bare-metal
stack. For an RTOS, provide a port implementation that returns a stable, non-NULL ID for
the current task or interrupt context. The profiler assigns up to 8 slots and keeps an
independent 16-frame stack for each. Call `profiler_context_switch(prev_id, next_id)` from
the context-switch hook so active durations exclude time a task is switched out; call
`profiler_context_release(id)` when a task is deleted so its slot can be reused. The
current context hook and switch integration must be safe to call from the profiling and
scheduler paths.

### Platform port contract

Implement `port/profiler_port.h` for a custom timer. The required operations initialize
and read the free-running timestamp counter and enter/exit a critical section. The
default context-ID hook is supplied by the library. The `16bit_it` port additionally
requires low-counter, overflow-pending, timer-init, and overflow-ISR hooks; its critical
section preserves the prior interrupt mask. The `2x16bit` port requires low/high timer
reads and timer initialization.

## Transport commands

Call `profiler_process()` regularly from the application loop to service a selected
transport. Commands are line-oriented:

| Command | Action |
|---|---|
| `prof-help` | List commands |
| `prof-status` | Show enabled state and timer frequency |
| `prof-top` | Write the sorted ASCII table |
| `prof-dump` | Write a PROF-BIN v4 dump |
| `prof-reset` | Clear metrics and restart profiling |

The legacy command `live` is also accepted as an alias for `prof-top`. `prof-top` and
`prof-dump` stop measurement for the response and restart it afterward.

The default UART transport buffers are 256 bytes for TX and 32 bytes for RX. DMA
integrations may provide custom static buffers with `profiler_transport_init_custom()`;
ensure they meet the target DMA's alignment and memory-access requirements. Check
`profiler_transport_status()` when the application needs to handle transport failures.

## PROF-BIN format and analysis

PROF-BIN is little-endian. Version 4 has a 28-byte header, zero or more 24-byte metric
records, and a 4-byte IEEE 802.3 CRC-32 trailer covering the header and records.

| Offset | Size | Field |
|---|---:|---|
| 0 | 4 | `PROF` magic |
| 4 | 2 | Version (`4`) |
| 6 | 2 | Metric record count |
| 8 | 4 | Timer frequency in Hz |
| 12 | 2 | Functions dropped because capacity was exhausted |
| 14 | 2 | Stack overflows |
| 16 | 2 | Unmatched exits |
| 18 | 2 | Stale frames discarded |
| 20 | 2 | Context overflows |
| 22 | 1 | Time mode (`1` = active cycles) |
| 23 | 1 | Reserved |
| 24 | 2 | Calibrated probe overhead in cycles |
| 26 | 2 | Calibrated nested-probe overhead in cycles |
| 28 | 24 per record | Address, calls, total/minimum/maximum cycles |
| End | 4 | CRC-32 |

The dump parser also accepts versions 2 and 3. It verifies the CRC, can resolve addresses
against an ELF using `arm-none-eabi-addr2line`, writes a detailed CSV, and optionally
prints a summary:

```powershell
python .\parse_prof_dump.py .\capture.bin --elf .\firmware.elf --summary
python .\parse_prof_dump.py .\capture.bin --elf .\firmware.elf --output .\metrics.csv
python .\parse_prof_dump.py .\top.log --text --elf .\firmware.elf --output .\top_resolved.txt
```

`--text` resolves the first address field of captured CSV/TOP rows. ARM Thumb addresses
are normalized for symbol lookup only; the original addresses remain in binary CSV output.
The parser needs no ELF if raw addresses are sufficient. Use a binary-capable serial
capture tool for `prof-dump`; terminal text logs are not suitable for capturing binary
frames.
