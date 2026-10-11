# profiler

Ultra-low-overhead, hardware-independent GCC function execution profiler module written in ANSI/ISO C99.

## Overview
The `profiler` module implements GCC's function instrumentation hooks (`__cyg_profile_func_enter` and `__cyg_profile_func_exit`) using an in-memory statistical aggregation model driven by `hashmap`. Rather than streaming raw chronological event traces that rapidly overflow circular buffers, `profiler` accumulates invocation counts, total execution cycles, minimum cycles, and maximum cycles per unique function directly in RAM.

### Key Architectural Highlights:
- **Zero OS / Hardware Coupling**: Completely hardware-independent core. No CMSIS, vendor, or RTOS dependencies.
- **Zero Dynamic Allocation**: Freestanding operation over caller-allocated `HashMapEntry`, `ProfilerMetric`, and `ProfilerStackFrame` buffers (MISRA C:2012 Rule 21.3).
- **In-Memory Cycle Accounting**: Tracks `call_count`, `total_cycles`, `min_cycles`, and `max_cycles` per unique function code address.
- **Sub-Microsecond In-Place Accumulation**: Uses Knuth's multiplicative golden ratio hash and in-place reference lookup (`hashmap_get_ref`) to locate and update accumulators in single-digit CPU cycles.
- **Per-Context Shadow Stacks**: One shadow stack per execution context (RTOS task or ISR), so enter/exit events of preempted tasks never mis-pair. A single shared stack (legacy mode) is kept for bare-metal builds.
- **Wall or Active Time**: Durations either include preemption/blocking (`PROFILER_TIME_WALL`) or count only the time the owning context was running (`PROFILER_TIME_ACTIVE`).
- **Packed Binary Protocol (`PROF-BIN v3`)**: Emits compact binary frames directly over `Stream`, with pairing diagnostics in the header, sealed with an IEEE 802.3 CRC-32 trailer.
- **Anti-Recursion Protection**: All module symbols decorated with `NO_INST` (`__attribute__((no_instrument_function))`).

---

## Build System Integration

Add the module to a CMake project and link the exported `profiler` target. The transport is selected
at configure time. The default `null` backend discards output; `memory` supports host tests;
`uart_stm32_hal_dma` is for DMA-capable STM32 integrations; and `uart_stm32_hal_it` uses UART
interrupt reception and blocking transmission for environments where DMA is unavailable, such as
Renode's current STM32G0 model.

```cmake
set(PROFILER_TRANSPORT "uart_stm32_hal_dma" CACHE STRING "")
add_subdirectory(path/to/profiler profiler_build)
target_link_libraries(firmware PRIVATE profiler)
```

The target exports `profiler.h`, `port/profiler_port.h`, and `port/profiler_transport.h`, and
resolves the `hashmap` and `stream` dependencies from `modules/` or sibling repositories. Set
`HASHMAP_DIR` or `STREAM_DIR` to override those locations. The library disables function
instrumentation for its own sources; instrument only application sources.

When using the STM32 backend, link it to the project's HAL interface target so CubeMX headers and
HAL symbols are available:

```cmake
target_link_libraries(profiler PRIVATE stm32cubemx)
```

The UART backend defines the three global HAL UART callbacks it uses. Do not define
`HAL_UART_TxCpltCallback`, `HAL_UART_ErrorCallback`, or `HAL_UARTEx_RxEventCallback` in the
application while that backend is selected.

---

## API Summary

```c
#include "profiler.h"
#include "hashmap.h"
#include "stream.h"

/* 1. Allocate static buffers with power-of-two map capacity */
#define MAP_CAPACITY     (64U)
#define METRICS_CAPACITY (48U) /* 75% load limit */
#define STACK_DEPTH      (16U)

static HashMapEntry       s_map_entries[MAP_CAPACITY];
static ProfilerMetric     s_metrics[METRICS_CAPACITY];
static ProfilerStackFrame s_stack_frames[STACK_DEPTH];

/* 2. Configure and initialize */
ProfilerConfig config = {
    .frequency        = 168000000U, /* e.g., 168 MHz CPU ticks */
    .map_entries      = s_map_entries,
    .map_capacity     = MAP_CAPACITY,
    .metrics          = s_metrics,
    .metrics_capacity = METRICS_CAPACITY,
    .stack_frames     = s_stack_frames,
    .stack_depth      = STACK_DEPTH
};

profiler_init(&config);

/* 3. Start profiling */
profiler_start();

/* ... run instrumented functions ... */

/* 4. Stream binary dump over abstract Stream transport (e.g. UART DMA) */
profiler_dump(&my_stream);

/* 5. Stop or reset if needed */
profiler_stop();
profiler_reset();
```

Leaving `contexts` NULL (or `context_capacity` 0) selects **legacy mode**: one shared stack of
`stack_depth` frames, exactly as in earlier releases. Use it only for bare-metal code without
preemption.

`profiler_start()` begins a new epoch: when hooks go from disabled to enabled, every shadow stack
is emptied, so exits of calls entered before a stop or dump are counted as unmatched instead of
being paired with stale frames. `profiler_dump()` does the same if the profiler was running.

---

## Execution Contexts and Time Modes

With an RTOS, every task has its own call stack, so the profiler needs one shadow stack per
context. Provide a context pool and a frame pool of `context_capacity * stack_depth` frames:

```c
#define CONTEXT_CAPACITY (6U)  /* pre-scheduler + tasks + idle + ISR */
#define STACK_DEPTH      (16U) /* per context */

static ProfilerStackFrame s_frames[CONTEXT_CAPACITY * STACK_DEPTH];
static ProfilerContext    s_contexts[CONTEXT_CAPACITY];

ProfilerConfig config = {
    /* ... map and metrics as above ... */
    .stack_frames     = s_frames,
    .stack_depth      = STACK_DEPTH,
    .contexts         = s_contexts,
    .context_capacity = CONTEXT_CAPACITY,
    .time_mode        = PROFILER_TIME_WALL /* or PROFILER_TIME_ACTIVE */
};
```

The context of each hook comes from the port function `profiler_port_context_id()`. The library
ships a weak default that returns NULL (one context). Slots are bound to IDs on first use; when all
slots are in use, the event is dropped and `context_overflows` is incremented.

| Mode | Duration of a call | Requires |
|---|---|---|
| `PROFILER_TIME_WALL` (default) | exit - enter, including preemption and blocking | `profiler_port_context_id()` |
| `PROFILER_TIME_ACTIVE` | exit - enter minus the time the context was switched out, and minus instrumented ISR time that preempted it | context-switch notifications |

### RTOS integration recipe

1. Implement `profiler_port_context_id()` in a file compiled with `-fno-instrument-functions`.
   Return `PROFILER_CONTEXT_ISR` in handler mode and the current task handle otherwise. Do not
   call instrumented RTOS APIs from it; cache the current task in the switch hook instead.
2. From the RTOS context-switch hook (interrupts masked), call
   `profiler_context_switch(prev_task, next_task)`. It is a no-op in legacy mode or while stopped.
3. When a task is deleted, call `profiler_context_release(task)` so its slot can be reused.

```c
static const void* volatile s_current_task = NULL;

NO_INST const void* profiler_port_context_id(void)
{
    return (__get_IPSR() != 0U) ? PROFILER_CONTEXT_ISR : s_current_task;
}

NO_INST void app_switch_hook(SertosTaskHandle prev, SertosTaskHandle next)
{
    s_current_task = next;
    profiler_context_switch(prev, next);
}
```

See `sertos_stm32g070/app/profiler_context_sertos.c` for the SerTOS version.

### Diagnostics

| Counter | API | Meaning |
|---|---|---|
| `unmatched_exits` | `profiler_unmatched_exit_count()` | Exit with no matching frame in its context (for example, the call started before `profiler_start()`) |
| `stale_frames` | `profiler_stale_frame_count()` | Frames above a matched exit that were discarded (missing exits, `longjmp`) |
| `context_overflows` | `profiler_context_overflow_count()` | Events dropped because no context slot was free |

All counters saturate at 0xFFFF and are cleared by `profiler_reset()`.

---

## Transport Integration

The application owns the command protocol and profiler start/stop lifecycle. The selected
transport backend supplies a `Stream` for `profiler_dump()` and exposes received frames through
`profiler_transport_poll()` and `Stream.read`.

Allocate transport buffers with static lifetime. DMA backends may require alignment and placement in
DMA-reachable memory:

```c
static uint8_t s_tx_buffer[256U] __attribute__((aligned(32)));
static uint8_t s_rx_buffer[32U] __attribute__((aligned(32)));

ProfilerTransportConfig transport_config = {
    .device = &huart2,
    .tx_buffer = s_tx_buffer,
    .tx_buffer_size = sizeof(s_tx_buffer),
    .rx_buffer = s_rx_buffer,
    .rx_buffer_size = sizeof(s_rx_buffer),
    .tx_timeout_ms = 1000U,
    .tick_ms = profiler_tick_ms,
    .yield = profiler_yield_ms
};

if (profiler_transport_init(&transport_config) != PROFILER_TRANSPORT_STATUS_OK) {
    Error_Handler();
}
if (profiler_transport_listen() != PROFILER_TRANSPORT_STATUS_OK) {
    Error_Handler();
}
```

In task context, poll the received frame, handle the command, call `profiler_dump()` with
`profiler_transport_stream()`, check `profiler_transport_status()` after the dump to detect a
flush failure, and re-arm reception. Keep HAL callbacks limited to latching transport events.

---

## Porting & Platform Backends

The module provides modular platform port backends selected via `PROFILER_PORT` in CMake:
- `dwt`: Native ARM Cortex-M DWT `CYCCNT` (M3/M4/M7/M33/M55) with zero interrupt overhead.
- `16bit_it`: Single 16-bit hardware timer extended with software overflow ISR (`profiler_port_16bit_it_overflow_isr`). The application also implements `profiler_port_hardware_timer16_overflow_pending()` (e.g. return `TIMx_SR.UIF`) so timestamps taken with interrupts masked (context-switch hook in PendSV, critical sections) account for a wrap whose ISR has not run yet. Ideal for Cortex-M0/M0+, MSP430, AVR, and Renode simulation.
- `2x16bit`: Cascaded Master/Slave 16-bit timers with rollover double-read glitch protection (e.g. STM32 TIM1+TIM3 hardware TRGO).
- `win`: Native Windows host port using `QueryPerformanceCounter` and `CRITICAL_SECTION`.
- `posix`: POSIX host port using `clock_gettime(CLOCK_MONOTONIC)` and `pthread_mutex`.
- `riscv`: RISC-V hardware port reading CSR `mcycle` and atomic `mstatus` MIE masking.
- `custom` (default): Application provides its own `profiler_port.c` implementing `port/profiler_port.h`.

```cmake
set(PROFILER_PORT "dwt" CACHE STRING "")
set(PROFILER_TRANSPORT "uart_stm32_hal_dma" CACHE STRING "")
add_subdirectory(path/to/profiler profiler_build)
target_link_libraries(firmware PRIVATE profiler)
```

---

## Analyzing a Dump

`parse_prof_dump.py` decodes the `PROF-BIN v2` and `v3` binary formats, verifies payload CRC-32 integrity, and resolves function addresses using `arm-none-eabi-addr2line`:

```bash
python parse_prof_dump.py dump.bin --elf firmware.elf --output report.csv --summary
```

The CSV columns are the same for both versions. For v3 dumps the parser also prints the time mode
and the diagnostic counters on stderr (and in `--summary`).

### PROF-BIN v3 layout (little endian)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `PROF` |
| 4 | 2 | version (`0x0003`) |
| 6 | 2 | metric count |
| 8 | 4 | timer frequency (Hz) |
| 12 | 2 | dropped functions (map full) |
| 14 | 2 | stack overflows |
| 16 | 2 | unmatched exits |
| 18 | 2 | stale frames |
| 20 | 2 | context overflows |
| 22 | 1 | time mode (0 wall, 1 active) |
| 23 | 1 | reserved |
| 24 | n * 24 | metrics (`ProfilerMetric`) |
| end | 4 | CRC-32 of everything before it |

For interactive visualization, see [Perfetto UI](https://ui.perfetto.dev/) for traces or
[Speedscope](https://www.speedscope.app/) for profiles. The profiler's binary dump and the parser's
CSV output are not directly documented as compatible with either viewer; conversion to a supported
trace or profile format may be needed.