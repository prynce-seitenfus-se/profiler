#include "profiler.h"
#include "profiler_port.h"
#include "profiler_transport.h"
#include "stream.h"
#include "hashmap.h"
#include <string.h>
#include <stdio.h>

/* Weak fallback for builds without a transport backend */
#ifndef PROFILER_HAS_TRANSPORT
__attribute__((weak)) NO_INST ProfilerTransportStatus profiler_transport_init(void* device)
{
    (void)device;
    return PROFILER_TRANSPORT_STATUS_OK;
}

__attribute__((weak)) NO_INST bool profiler_transport_process(void)
{
    return false;
}
#endif

#ifndef PROFILER_CAPACITY_FUNCTIONS
#define PROFILER_CAPACITY_FUNCTIONS (128U)
#endif
#ifndef PROFILER_CAPACITY_STACK
#define PROFILER_CAPACITY_STACK     (16U)
#endif
#ifndef PROFILER_CAPACITY_CONTEXTS
#define PROFILER_CAPACITY_CONTEXTS  (8U)
#endif
#if (PROFILER_CAPACITY_FUNCTIONS == 0U) || \
    ((PROFILER_CAPACITY_FUNCTIONS & (PROFILER_CAPACITY_FUNCTIONS - 1U)) != 0U) || \
    (PROFILER_CAPACITY_FUNCTIONS > 65535U) || \
    (PROFILER_CAPACITY_STACK == 0U) || (PROFILER_CAPACITY_CONTEXTS == 0U)
#error "Profiler capacities require a power-of-two function count (1..32768) and nonzero stack/context counts."
#endif

/* Internal metric record keyed by function address */
typedef struct ProfilerInternalMetric {
    uint32_t    fn_address;   /**< Unique function code address (key). */
    uint32_t    call_count;   /**< Total number of completed invocations. */
    uint64_t    total_cycles; /**< Accumulated execution time in CPU cycles. */
    uint32_t    min_cycles;   /**< Minimum observed execution duration in cycles. */
    uint32_t    max_cycles;   /**< Maximum observed execution duration in cycles. */
} ProfilerInternalMetric;

/* Binary protocol record format (matches PROF-BIN v4: 24 bytes) */
typedef struct ProfilerBinRecord {
    uint32_t fn_address;
    uint32_t call_count;
    uint64_t total_cycles;
    uint32_t min_cycles;
    uint32_t max_cycles;
} ProfilerBinRecord;

/* Binary protocol header for streaming dumps (PROF-BIN v4, 28 bytes) */
typedef struct ProfilerBinHeader {
    uint32_t magic;             /**< "PROF" magic identifier. */
    uint16_t version;           /**< Protocol version (0x0004U). */
    uint16_t record_count;      /**< Number of binary metric records following header. */
    uint32_t frequency;         /**< Timestamp clock frequency in Hz. */
    uint16_t dropped_functions; /**< Unique functions dropped due to capacity exhaustion. */
    uint16_t stack_overflows;   /**< Calls dropped due to shadow call stack overflow. */
    uint16_t unmatched_exits;   /**< Exits without a matching entry frame (saturating). */
    uint16_t stale_frames;      /**< Frames discarded because their exit was never seen (saturating). */
    uint16_t context_overflows; /**< Events dropped because no context slot was free (saturating). */
    uint8_t  time_mode;         /**< Active-time protocol marker (1). */
    uint8_t  reserved;          /**< Reserved, always 0. */
    uint16_t probe_overhead;    /**< Calibrated hook-window overhead subtracted from durations. */
    uint16_t nested_overhead;   /**< Calibrated untracked nested hook cost subtracted from callers. */
} ProfilerBinHeader;

/* Shadow call stack frame */
typedef struct ProfilerStackFrame {
    const void* fn_address;
    uint32_t    enter_timestamp;
    uint32_t    enter_suspended;
    uint32_t    enter_probe;
} ProfilerStackFrame;

/* Context state */
typedef struct ProfilerContext {
    const void*         id;
    ProfilerStackFrame* frames;
    size_t              index;
    size_t              overflow_depth;
    uint32_t            suspended_cycles;
    uint32_t            switch_out_ts;
    uint32_t            probe_cycles;
    bool                in_use;
    bool                switched_out;
} ProfilerContext;

/* Pre-allocated static storage (zero heap allocation, completely internal) */
static HashMap                s_map;
/* HashMap limits occupancy to 75%; reserve enough slots for every metric. */
static HashMapEntry           s_map_entries[PROFILER_CAPACITY_FUNCTIONS * 2U];
static ProfilerInternalMetric s_metrics[PROFILER_CAPACITY_FUNCTIONS];
static size_t                 s_metrics_count = 0U;

static ProfilerStackFrame     s_stack_pool[(PROFILER_CAPACITY_CONTEXTS + 1U) * PROFILER_CAPACITY_STACK];
static ProfilerContext        s_contexts[PROFILER_CAPACITY_CONTEXTS];
static ProfilerContext        s_legacy_context;

static uint32_t               s_frequency = 0U;
static volatile bool          s_enabled = false;
static uint16_t               s_dropped_functions = 0U;
static uint16_t               s_stack_overflows = 0U;
static uint16_t               s_unmatched_exits = 0U;
static uint16_t               s_stale_frames = 0U;
static uint16_t               s_context_overflows = 0U;
static uint32_t               s_last_wall_ticks = 0U;
static uint64_t               s_elapsed_wall_ticks = 0ULL;

/* Calibration constants */
#define PROFILER_CALIBRATION_DEPTH (2U)
static uint32_t               s_probe_overhead = 0U;
static uint32_t               s_nested_overhead = 0U;
static volatile bool          s_calibrating = false;
static volatile bool          s_calibration_cancelled = false;
static const void*            s_calibration_owner = NULL;
static uint32_t               s_calibration_probe = 0U;
static uint32_t               s_calibration_nested = 0U;
static uint32_t               s_calibration_min = 0U;
static ProfilerContext        s_calibration_context;
static ProfilerStackFrame     s_calibration_frames[PROFILER_CALIBRATION_DEPTH];
static const uint8_t          s_calibration_ids[PROFILER_CALIBRATION_DEPTH] = { 0U, 0U };

/* IEEE 802.3 CRC-32 (0xEDB88320) */
NO_INST static uint32_t calculate_crc32(uint32_t seed, const uint8_t* data, size_t length)
{
    uint32_t crc = seed;

    if (data == NULL) {
        return crc;
    }

    for (size_t i = 0U; i < length; ++i) {
        crc ^= (uint32_t)data[i];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1U) ^ 0xEDB88320U;
            } else {
                crc >>= 1U;
            }
        }
    }

    return crc;
}

NO_INST static void saturating_increment(uint16_t* counter)
{
    if (*counter < 0xFFFFU) {
        (*counter)++;
    }
}

NO_INST static void reset_context(ProfilerContext* ctx, bool unbind)
{
    ctx->index = 0U;
    ctx->overflow_depth = 0U;
    ctx->switched_out = false;
    if (unbind) {
        ctx->id = NULL;
        ctx->in_use = false;
        ctx->suspended_cycles = 0U;
        ctx->switch_out_ts = 0U;
        ctx->probe_cycles = 0U;
    }
}

NO_INST static void reset_all_contexts(bool unbind)
{
    s_legacy_context.index = 0U;
    s_legacy_context.overflow_depth = 0U;
    s_legacy_context.switched_out = false;

    for (size_t i = 0U; i < PROFILER_CAPACITY_CONTEXTS; ++i) {
        reset_context(&s_contexts[i], unbind);
    }
}

NO_INST static ProfilerContext* lookup_context(const void* id)
{
    for (size_t i = 0U; i < PROFILER_CAPACITY_CONTEXTS; ++i) {
        if (s_contexts[i].in_use && (s_contexts[i].id == id)) {
            return &s_contexts[i];
        }
    }
    return NULL;
}

NO_INST static ProfilerContext* bind_context(const void* id)
{
    for (size_t i = 0U; i < PROFILER_CAPACITY_CONTEXTS; ++i) {
        ProfilerContext* ctx = &s_contexts[i];
        if (!ctx->in_use) {
            reset_context(ctx, true);
            ctx->id = id;
            ctx->in_use = true;
            return ctx;
        }
    }

    saturating_increment(&s_context_overflows);
    return NULL;
}

NO_INST static ProfilerContext* find_context(const void* id)
{
    ProfilerContext* ctx = lookup_context(id);
    if (ctx == NULL) {
        ctx = bind_context(id);
    }
    return ctx;
}

NO_INST static ProfilerContext* current_context(void)
{
    const void* cid = profiler_port_context_id();
    if (cid == NULL) {
        return &s_legacy_context;
    }
    return find_context(cid);
}

NO_INST static void clear_state(void)
{
    s_enabled = false;
    s_metrics_count = 0U;
    (void)memset(&s_legacy_context, 0, sizeof(s_legacy_context));
    (void)memset(s_contexts, 0, sizeof(s_contexts));
    (void)memset(s_metrics, 0, sizeof(s_metrics));
    (void)memset(s_map_entries, 0, sizeof(s_map_entries));
    s_frequency = 0U;
    s_dropped_functions = 0U;
    s_stack_overflows = 0U;
    s_unmatched_exits = 0U;
    s_stale_frames = 0U;
    s_context_overflows = 0U;
    s_probe_overhead = 0U;
    s_nested_overhead = 0U;
    s_calibrating = false;
    s_calibration_cancelled = false;
    s_calibration_owner = NULL;
    s_last_wall_ticks = 0U;
    s_elapsed_wall_ticks = 0ULL;
}

NO_INST bool profiler_init(uint32_t frequency, void* transport_device)
{
    if (frequency == 0U) {
        return false;
    }

    clear_state();

    if (!hashmap_init(&s_map, s_map_entries, PROFILER_CAPACITY_FUNCTIONS * 2U)) {
        return false;
    }

    s_frequency = frequency;

    s_legacy_context.frames = s_stack_pool;
    s_legacy_context.in_use = true;

    for (size_t i = 0U; i < PROFILER_CAPACITY_CONTEXTS; ++i) {
        s_contexts[i].frames = &s_stack_pool[(i + 1U) * PROFILER_CAPACITY_STACK];
    }

    profiler_port_init();

    if (transport_device != NULL) {
        if (profiler_transport_init(transport_device) != PROFILER_TRANSPORT_STATUS_OK) {
            return false;
        }
    }

    return true;
}

NO_INST static bool calibrate_probe(uint32_t* probe, uint32_t* nested)
{
    (void)memset(&s_calibration_context, 0, sizeof(s_calibration_context));
    s_calibration_context.frames = s_calibration_frames;
    s_calibration_context.in_use = true;
    s_calibration_context.id = s_calibration_owner;

    s_calibration_probe = 0U;
    s_calibration_nested = 0U;
    s_calibration_min = UINT32_MAX;

    for (size_t i = 0U; i < 16U; ++i) {
        uint32_t state = profiler_port_enter_critical();
        if (s_calibration_cancelled) {
            profiler_port_exit_critical(state);
            return false;
        }
        __cyg_profile_func_enter((void*)&s_calibration_ids[0], NULL);
        __cyg_profile_func_exit((void*)&s_calibration_ids[0], NULL);
        profiler_port_exit_critical(state);
    }
    *probe = (s_calibration_min == UINT32_MAX) ? 0U : s_calibration_min;
    s_calibration_probe = *probe;

    s_calibration_min = UINT32_MAX;
    for (size_t i = 0U; i < 16U; ++i) {
        uint32_t state = profiler_port_enter_critical();
        if (s_calibration_cancelled) {
            profiler_port_exit_critical(state);
            return false;
        }
        __cyg_profile_func_enter((void*)&s_calibration_ids[0], NULL);
        __cyg_profile_func_enter((void*)&s_calibration_ids[1], NULL);
        __cyg_profile_func_exit((void*)&s_calibration_ids[1], NULL);
        __cyg_profile_func_exit((void*)&s_calibration_ids[0], NULL);
        profiler_port_exit_critical(state);
    }
    *nested = (s_calibration_min == UINT32_MAX) ? 0U : s_calibration_min;
    return true;
}

NO_INST void profiler_start(void)
{
    uint32_t state = profiler_port_enter_critical();
    if (!s_enabled && !s_calibrating && (s_frequency != 0U)) {
        reset_all_contexts(false);
        if (s_probe_overhead == 0U) {
            s_calibration_owner = profiler_port_context_id();
            s_calibration_cancelled = false;
            s_calibrating = true;
            profiler_port_exit_critical(state);

            uint32_t probe = 0U;
            uint32_t nested = 0U;
            bool calibrated = calibrate_probe(&probe, &nested);

            state = profiler_port_enter_critical();
            if (calibrated && !s_calibration_cancelled) {
                s_probe_overhead = probe;
                s_nested_overhead = nested;
                s_last_wall_ticks = profiler_port_ticks();
                s_enabled = true;
            }
            s_calibrating = false;
            s_calibration_owner = NULL;
        } else {
            s_last_wall_ticks = profiler_port_ticks();
            s_enabled = true;
        }
    }
    profiler_port_exit_critical(state);
}

NO_INST void profiler_stop(void)
{
    uint32_t state = profiler_port_enter_critical();
    if (s_calibrating) {
        s_calibration_cancelled = true;
    }
    if (s_enabled) {
        uint32_t now = profiler_port_ticks();
        s_elapsed_wall_ticks += (uint64_t)(now - s_last_wall_ticks);
        s_last_wall_ticks = now;
        s_enabled = false;
    }
    profiler_port_exit_critical(state);
}

NO_INST void profiler_reset(void)
{
    uint32_t state = profiler_port_enter_critical();
    if (s_calibrating) {
        s_calibration_cancelled = true;
    }
    s_enabled = false;
    s_metrics_count = 0U;
    s_dropped_functions = 0U;
    s_stack_overflows = 0U;
    s_unmatched_exits = 0U;
    s_stale_frames = 0U;
    s_context_overflows = 0U;
    s_elapsed_wall_ticks = 0ULL;
    s_last_wall_ticks = profiler_port_ticks();
    (void)memset(s_metrics, 0, sizeof(s_metrics));
    hashmap_clear(&s_map);
    reset_all_contexts(true);
    profiler_port_exit_critical(state);
}

NO_INST bool profiler_enabled(void)
{
    return s_enabled;
}

NO_INST uint32_t profiler_frequency(void)
{
    return s_frequency;
}

NO_INST void profiler_context_switch(const void* prev_id, const void* next_id)
{
    if (!s_enabled) {
        return;
    }

    uint32_t ts = profiler_port_ticks();
    uint32_t state = profiler_port_enter_critical();

    ProfilerContext* prev = lookup_context(prev_id);
    if ((prev != NULL) && (prev_id != next_id)) {
        prev->switch_out_ts = ts;
        prev->switched_out = true;
    }

    ProfilerContext* next = find_context(next_id);
    if ((next != NULL) && next->switched_out) {
        next->suspended_cycles += profiler_port_ticks() - next->switch_out_ts;
        next->switched_out = false;
    }

    profiler_port_exit_critical(state);
}

NO_INST void profiler_context_release(const void* id)
{
    uint32_t state = profiler_port_enter_critical();
    ProfilerContext* ctx = lookup_context(id);
    if (ctx != NULL) {
        reset_context(ctx, true);
    }
    profiler_port_exit_critical(state);
}

NO_INST static void update_metric(const void* fn_addr, uint32_t cycles)
{
    void** slot = hashmap_get_ref(&s_map, fn_addr);

    if (slot != NULL) {
        ProfilerInternalMetric* metric = (ProfilerInternalMetric*)(*slot);
        metric->call_count++;
        metric->total_cycles += (uint64_t)cycles;
        if (cycles < metric->min_cycles) {
            metric->min_cycles = cycles;
        }
        if (cycles > metric->max_cycles) {
            metric->max_cycles = cycles;
        }
    } else if (s_metrics_count < PROFILER_CAPACITY_FUNCTIONS) {
        ProfilerInternalMetric* metric = &s_metrics[s_metrics_count];
        metric->fn_address = (uint32_t)(uintptr_t)fn_addr;
        metric->call_count = 1U;
        metric->total_cycles = (uint64_t)cycles;
        metric->min_cycles = cycles;
        metric->max_cycles = cycles;

        if (hashmap_insert(&s_map, fn_addr, (void*)metric)) {
            s_metrics_count++;
        } else {
            saturating_increment(&s_dropped_functions);
        }
    } else {
        saturating_increment(&s_dropped_functions);
    }
}

NO_INST static size_t find_frame(const ProfilerContext* ctx, const void* fn)
{
    for (size_t i = ctx->index; i > 0U; --i) {
        if (ctx->frames[i - 1U].fn_address == fn) {
            return i;
        }
    }
    return 0U;
}

NO_INST static uint32_t frame_cycles(const ProfilerContext* ctx, const ProfilerStackFrame* frame, uint32_t ts)
{
    uint32_t cycles = ts - frame->enter_timestamp;
    uint32_t probe = ctx->probe_cycles - frame->enter_probe;
    cycles = (probe < cycles) ? (cycles - probe) : 0U;

    uint32_t suspended = ctx->suspended_cycles - frame->enter_suspended;
    cycles = (suspended < cycles) ? (cycles - suspended) : 0U;

    return cycles;
}

NO_INST static void record_call(const ProfilerContext* ctx, const void* fn, uint32_t cycles, uint32_t overhead)
{
    uint32_t net = (overhead < cycles) ? (cycles - overhead) : 0U;

    if (ctx != &s_calibration_context) {
        update_metric(fn, net);
    } else if ((fn == (const void*)&s_calibration_ids[0]) && (net < s_calibration_min)) {
        s_calibration_min = net;
    }
}

NO_INST static void pop_context_frame(ProfilerContext* ctx, const void* fn, uint32_t ts, uint32_t overhead)
{
    size_t match_pos = find_frame(ctx, fn);
    if (match_pos == 0U) {
        saturating_increment(&s_unmatched_exits);
        return;
    }

    size_t match_idx = match_pos - 1U;
    uint32_t cycles = frame_cycles(ctx, &ctx->frames[match_idx], ts);
    record_call(ctx, fn, cycles, overhead);

    size_t discarded = ctx->index - match_pos;
    if (discarded > 0U) {
        saturating_increment(&s_stale_frames);
    }
    ctx->index = match_idx;
}

NO_INST static ProfilerContext* hook_context(void)
{
    if (s_calibrating && (profiler_port_context_id() == s_calibration_owner)) {
        return &s_calibration_context;
    }
    if (!s_enabled) {
        return NULL;
    }
    return current_context();
}

NO_INST __attribute__((noinline)) void __cyg_profile_func_enter(void* this_fn, void* call_site)
{
    const void* id = this_fn;
    (void)call_site;
    if ((!s_enabled && !s_calibrating) || (id == NULL)) {
        return;
    }

    uint32_t state = profiler_port_enter_critical();
    uint32_t start = profiler_port_ticks();
    ProfilerContext* ctx = hook_context();

    if (ctx != NULL) {
        size_t depth = (ctx == &s_calibration_context) ? PROFILER_CALIBRATION_DEPTH : PROFILER_CAPACITY_STACK;
        ProfilerStackFrame* frame = NULL;
        if ((ctx->overflow_depth == 0U) && (ctx->index < depth)) {
            frame = &ctx->frames[ctx->index];
            frame->fn_address = id;
            frame->enter_suspended = ctx->suspended_cycles;
            ctx->index++;
        } else {
            if (ctx->overflow_depth < SIZE_MAX) {
                ctx->overflow_depth++;
            }
            saturating_increment(&s_stack_overflows);
        }

        uint32_t ts = profiler_port_ticks();
        ctx->probe_cycles += ts - start;
        if (frame != NULL) {
            frame->enter_probe = ctx->probe_cycles;
            frame->enter_timestamp = ts;
        }
    }

    profiler_port_exit_critical(state);
}

NO_INST __attribute__((noinline)) void __cyg_profile_func_exit(void* this_fn, void* call_site)
{
    const void* id = this_fn;
    (void)call_site;
    if ((!s_enabled && !s_calibrating) || (id == NULL)) {
        return;
    }

    uint32_t state = profiler_port_enter_critical();
    uint32_t ts = profiler_port_ticks();
    ProfilerContext* ctx = hook_context();

    if ((ctx != NULL) && (ctx->overflow_depth > 0U)) {
        ctx->overflow_depth--;
        ctx->probe_cycles += (profiler_port_ticks() - ts) + s_nested_overhead;
        profiler_port_exit_critical(state);
        return;
    }

    if (ctx == &s_calibration_context) {
        pop_context_frame(ctx, id, ts, s_calibration_probe);
        ctx->probe_cycles += (profiler_port_ticks() - ts) + s_calibration_nested;
    } else if (ctx != NULL) {
        pop_context_frame(ctx, id, ts, s_probe_overhead);
        ctx->probe_cycles += (profiler_port_ticks() - ts) + s_nested_overhead;
    }

    profiler_port_exit_critical(state);
}

/* Addresses are resolved against the matching ELF by host tools. */
NO_INST static void format_fn_name(const ProfilerInternalMetric* m, char* out_buf, size_t max_len)
{
    (void)snprintf(out_buf, max_len, "0x%08lX", (unsigned long)m->fn_address);
}

/* Dump telemetry in BIN, CSV, or TOP format */
NO_INST bool profiler_dump(const struct Stream* stream, ProfilerDumpFormat format)
{
    if (stream == NULL) {
        return false;
    }

    uint32_t state = profiler_port_enter_critical();
    bool was_enabled = s_enabled;
    if (s_enabled) {
        uint32_t now = profiler_port_ticks();
        s_elapsed_wall_ticks += (uint64_t)(now - s_last_wall_ticks);
        s_last_wall_ticks = now;
        s_enabled = false;
    }
    profiler_port_exit_critical(state);

    bool success = true;

    if (format == PROFILER_DUMP_BIN) {
        ProfilerBinHeader header;
        (void)memset(&header, 0, sizeof(header));
        header.magic = PROFILER_BIN_MAGIC;
        header.version = PROFILER_BIN_VERSION;
        header.record_count = (uint16_t)s_metrics_count;
        header.frequency = s_frequency;
        header.dropped_functions = s_dropped_functions;
        header.stack_overflows = s_stack_overflows;
        header.unmatched_exits = s_unmatched_exits;
        header.stale_frames = s_stale_frames;
        header.context_overflows = s_context_overflows;
        header.time_mode = 1U; /* active */
        header.probe_overhead = (s_probe_overhead > 0xFFFFU) ? 0xFFFFU : (uint16_t)s_probe_overhead;
        header.nested_overhead = (s_nested_overhead > 0xFFFFU) ? 0xFFFFU : (uint16_t)s_nested_overhead;

        uint32_t crc = calculate_crc32(0xFFFFFFFFU, (const uint8_t*)&header, sizeof(header));

        /* Convert internal metrics to standard 24-byte records */
        size_t written = stream_write(stream, (const uint8_t*)&header, sizeof(header));
        success = (written == sizeof(header));

        for (size_t i = 0U; success && (i < s_metrics_count); ++i) {
            ProfilerBinRecord r;
            r.fn_address = s_metrics[i].fn_address;
            r.call_count = s_metrics[i].call_count;
            r.total_cycles = s_metrics[i].total_cycles;
            r.min_cycles = s_metrics[i].min_cycles;
            r.max_cycles = s_metrics[i].max_cycles;

            crc = calculate_crc32(crc, (const uint8_t*)&r, sizeof(r));
            written = stream_write(stream, (const uint8_t*)&r, sizeof(r));
            success = (written == sizeof(r));
        }

        crc ^= 0xFFFFFFFFU;
        if (success) {
            written = stream_write(stream, (const uint8_t*)&crc, sizeof(crc));
            success = (written == sizeof(crc));
        }
    } else if (format == PROFILER_DUMP_CSV) {
        static const char csv_header[] = "function,calls,min,max,avg\r\n";
        (void)stream_write(stream, (const uint8_t*)csv_header, sizeof(csv_header) - 1U);

        char line[128];
        char fn_str[36];
        for (size_t i = 0U; i < s_metrics_count; ++i) {
            format_fn_name(&s_metrics[i], fn_str, sizeof(fn_str));
            uint32_t avg = (s_metrics[i].call_count > 0U) ?
                (uint32_t)(s_metrics[i].total_cycles / (uint64_t)s_metrics[i].call_count) : 0U;

            int len = snprintf(line, sizeof(line), "%s,%lu,%lu,%lu,%lu\r\n",
                               fn_str,
                               (unsigned long)s_metrics[i].call_count,
                               (unsigned long)s_metrics[i].min_cycles,
                               (unsigned long)s_metrics[i].max_cycles,
                               (unsigned long)avg);
            if (len > 0) {
                (void)stream_write(stream, (const uint8_t*)line, (size_t)len);
            }
        }
    } else { /* PROFILER_DUMP_TOP */
        /* Sum total active cycles across all functions and count active entries */
        uint64_t total_active_cycles = 0ULL;
        uint64_t total_calls = 0ULL;
        size_t active_count = 0U;
        uint16_t order[PROFILER_CAPACITY_FUNCTIONS];

        for (size_t i = 0U; i < s_metrics_count; ++i) {
            if (s_metrics[i].call_count > 0U) {
                total_active_cycles += s_metrics[i].total_cycles;
                total_calls += s_metrics[i].call_count;
                order[active_count] = (uint16_t)i;
                active_count++;
            }
        }

        /* Stable insertion sort of active indices by total_cycles descending */
        for (size_t i = 1U; i < active_count; ++i) {
            uint16_t key = order[i];
            size_t j = i;
            while ((j > 0U) && (s_metrics[order[j - 1U]].total_cycles < s_metrics[key].total_cycles)) {
                order[j] = order[j - 1U];
                j--;
            }
            order[j] = key;
        }

        /* Header banner with CPU frequency and measurement units */
        static const char banner_top[] =
            "\r\n=== PROFILER RUNTIME METRICS [CPU: ";
        static const char banner_cols[] =
            "] ==================\r\n"
            "Function                         Calls     Total (kcyc)   %Work    Min    Max    Avg   Jitter\r\n"
            "-------------------------------------------------------------------------------------------\r\n";
        (void)stream_write(stream, (const uint8_t*)banner_top, sizeof(banner_top) - 1U);

        char freq_str[32];
        uint32_t freq_mhz_x10 = (s_frequency > 0U) ? (s_frequency / 100000U) : 0U;
        int freq_len = snprintf(freq_str, sizeof(freq_str), "%lu.%1lu MHz | Units: Cycles",
            (unsigned long)(freq_mhz_x10 / 10U),
            (unsigned long)(freq_mhz_x10 % 10U));
        if (freq_len > 0) {
            (void)stream_write(stream, (const uint8_t*)freq_str, (size_t)freq_len);
        }
        (void)stream_write(stream, (const uint8_t*)banner_cols, sizeof(banner_cols) - 1U);

        /* Rows in sorted order */
        char line[128];
        char fn_str[36];
        for (size_t idx = 0U; idx < active_count; ++idx) {
            size_t i = order[idx];
            format_fn_name(&s_metrics[i], fn_str, sizeof(fn_str));

            uint32_t avg = (s_metrics[i].call_count > 0U) ?
                (uint32_t)(s_metrics[i].total_cycles / (uint64_t)s_metrics[i].call_count) : 0U;
            uint32_t jitter = (s_metrics[i].max_cycles >= s_metrics[i].min_cycles) ?
                (s_metrics[i].max_cycles - s_metrics[i].min_cycles) : 0U;
            uint32_t pct_x10 = (total_active_cycles > 0ULL) ?
                (uint32_t)((s_metrics[i].total_cycles * 1000ULL) / total_active_cycles) : 0U;
            bool outlier = (jitter > (avg / 10U)) && (jitter > 5U);

            int len = snprintf(line, sizeof(line),
                "%-30s %8lu %16lu %4lu.%1lu%% %6lu %6lu %6lu   +%-5lu %s\r\n",
                fn_str,
                (unsigned long)s_metrics[i].call_count,
                (unsigned long)(s_metrics[i].total_cycles / 1000ULL),
                (unsigned long)(pct_x10 / 10U),
                (unsigned long)(pct_x10 % 10U),
                (unsigned long)s_metrics[i].min_cycles,
                (unsigned long)s_metrics[i].max_cycles,
                (unsigned long)avg,
                (unsigned long)jitter,
                outlier ? "!" : " ");
            if (len > 0) {
                (void)stream_write(stream, (const uint8_t*)line, (size_t)len);
            }
        }

        /* Summary and Health Diagnostic Footer */
        static const char div_line[] =
            "-------------------------------------------------------------------------------------------\r\n";
        (void)stream_write(stream, (const uint8_t*)div_line, sizeof(div_line) - 1U);

        char foot_buf[256];
        uint32_t sys_load_x10 = (s_elapsed_wall_ticks > 0ULL) ?
            (uint32_t)((total_active_cycles * 1000ULL) / s_elapsed_wall_ticks) : 0U;
        if (sys_load_x10 > 1000U) {
            sys_load_x10 = 1000U;
        }
        uint32_t idle_x10 = 1000U - sys_load_x10;

        uint32_t elapsed_sec_x100 = (s_frequency > 0U) ?
            (uint32_t)((s_elapsed_wall_ticks * 100ULL) / (uint64_t)s_frequency) : 0U;
        uint32_t total_mcyc_x10 = (uint32_t)(total_active_cycles / 100000ULL);

        int foot_len = snprintf(foot_buf, sizeof(foot_buf),
            "System Utilization: [Active Load: %lu.%1lu%% | Idle / Sleep: %lu.%1lu%%]\r\n"
            "Function Summary:   %u active functions | %lu calls | Active: %lu.%1luM cyc | Elapsed: %lu.%02lu s\r\n"
            "Health:             %u dropped | %u stack overflows | Calibrated probe overhead: %u cyc\r\n",
            (unsigned long)(sys_load_x10 / 10U),
            (unsigned long)(sys_load_x10 % 10U),
            (unsigned long)(idle_x10 / 10U),
            (unsigned long)(idle_x10 % 10U),
            (unsigned int)active_count,
            (unsigned long)total_calls,
            (unsigned long)(total_mcyc_x10 / 10U),
            (unsigned long)(total_mcyc_x10 % 10U),
            (unsigned long)(elapsed_sec_x100 / 100U),
            (unsigned long)(elapsed_sec_x100 % 100U),
            (unsigned int)s_dropped_functions,
            (unsigned int)s_stack_overflows,
            (unsigned int)s_probe_overhead);
        if (foot_len > 0) {
            (void)stream_write(stream, (const uint8_t*)foot_buf, (size_t)foot_len);
        }

        static const char end_bar[] =
            "===========================================================================================\r\n\r\n";
        (void)stream_write(stream, (const uint8_t*)end_bar, sizeof(end_bar) - 1U);
    }

    stream_flush(stream);

    state = profiler_port_enter_critical();
    if (was_enabled) {
        reset_all_contexts(false);
        s_last_wall_ticks = profiler_port_ticks();
        s_enabled = true;
    }
    profiler_port_exit_critical(state);

    return success;
}

NO_INST bool profiler_cmd_execute(const char* cmd, const struct Stream* stream)
{
    size_t first = 0U;
    size_t last;
    size_t len;
    const char* opt;
    size_t opt_len;

    if ((cmd == NULL) || (stream == NULL)) {
        return false;
    }

    last = strlen(cmd);

    /* Strip leading and trailing whitespace */
    while ((first < last) &&
           ((cmd[first] == ' ') || (cmd[first] == '\t') ||
            (cmd[first] == '\r') || (cmd[first] == '\n'))) {
        first++;
    }
    while ((last > first) &&
           ((cmd[last - 1U] == ' ') || (cmd[last - 1U] == '\t') ||
            (cmd[last - 1U] == '\r') || (cmd[last - 1U] == '\n'))) {
        last--;
    }

    len = last - first;

    /* Profiler commands start with "prof-" or legacy "live" */
    if ((len >= 5U) && (strncmp(&cmd[first], "prof-", 5U) == 0)) {
        opt = &cmd[first + 5U];
        opt_len = len - 5U;
    } else if ((len == 4U) && (strncmp(&cmd[first], "live", 4U) == 0)) {
        opt = &cmd[first];
        opt_len = 4U;
    } else {
        return false;
    }

    if ((opt_len == 4U) && (strncmp(opt, "dump", 4U) == 0)) {
        profiler_stop();
        (void)profiler_dump(stream, PROFILER_DUMP_BIN);
        profiler_start();
        return true;
    } else if (((opt_len == 3U) && (strncmp(opt, "top", 3U) == 0)) ||
               ((opt_len == 4U) && (strncmp(opt, "live", 4U) == 0))) {
        profiler_stop();
        (void)profiler_dump(stream, PROFILER_DUMP_TOP);
        profiler_start();
        return true;
    } else if ((opt_len == 5U) && (strncmp(opt, "reset", 5U) == 0)) {
        profiler_reset();
        profiler_start();
        static const char reset_msg[] = "OK: profiler reset\r\n";
        (void)stream_write(stream, (const uint8_t*)reset_msg, sizeof(reset_msg) - 1U);
        stream_flush(stream);
        return true;
    } else if ((opt_len == 6U) && (strncmp(opt, "status", 6U) == 0)) {
        char status_buf[128];
        int s_len = snprintf(status_buf, sizeof(status_buf),
            "\r\nProfiler Status:\r\n  Enabled:   %s\r\n  Frequency: %lu Hz\r\n\r\n",
            profiler_enabled() ? "YES" : "NO",
            (unsigned long)profiler_frequency());
        if (s_len > 0) {
            (void)stream_write(stream, (const uint8_t*)status_buf, (size_t)s_len);
            stream_flush(stream);
        }
        return true;
    } else if ((opt_len == 4U) && (strncmp(opt, "help", 4U) == 0)) {
        static const char help_msg[] =
            "\r\nProfiler Commands:\r\n"
            "  prof-top    - Dump ASCII cycle statistics table\r\n"
            "  prof-dump   - Stream binary telemetry (PROF-BIN)\r\n"
            "  prof-reset  - Reset accumulated cycle statistics\r\n"
            "  prof-status - Show profiler status and clock frequency\r\n"
            "  prof-help   - Print this command list\r\n\r\n";
        (void)stream_write(stream, (const uint8_t*)help_msg, sizeof(help_msg) - 1U);
        stream_flush(stream);
        return true;
    } else {
        static const char err_msg[] = "ERR: unknown profiler command (type 'prof-help')\r\n";
        (void)stream_write(stream, (const uint8_t*)err_msg, sizeof(err_msg) - 1U);
        stream_flush(stream);
        return true;
    }
}

NO_INST bool profiler_stream_process(const struct Stream* stream)
{
    char line_buffer[64];
    size_t line_len = 0U;
    uint8_t ch;
    bool handled = false;

    if (stream == NULL) {
        return false;
    }

    while (stream_read(stream, &ch, 1U) == 1U) {
        if ((ch == (uint8_t)'\r') || (ch == (uint8_t)'\n')) {
            if (line_len > 0U) {
                line_buffer[line_len] = '\0';
                if (profiler_cmd_execute(line_buffer, stream)) {
                    handled = true;
                }
                line_len = 0U;
            }
        } else {
            if (line_len < (sizeof(line_buffer) - 1U)) {
                line_buffer[line_len++] = (char)ch;
            } else {
                /* Discard oversized frame */
                line_len = 0U;
            }
        }
    }

    if (line_len > 0U) {
        line_buffer[line_len] = '\0';
        if (profiler_cmd_execute(line_buffer, stream)) {
            handled = true;
        }
    }

    return handled;
}

NO_INST bool profiler_process(void)
{
    uint32_t state = profiler_port_enter_critical();
    if (s_enabled) {
        uint32_t now = profiler_port_ticks();
        s_elapsed_wall_ticks += (uint64_t)(now - s_last_wall_ticks);
        s_last_wall_ticks = now;
    }
    profiler_port_exit_critical(state);
    return profiler_transport_process();
}
