#include "profiler.h"
#include "profiler_port.h"
#include "stream.h"
#include "hashmap.h"
#include <string.h>
#include <stdio.h>

#define PROFILER_CAPACITY_FUNCTIONS (128U)
#define PROFILER_CAPACITY_STACK     (16U)
#define PROFILER_CAPACITY_CONTEXTS  (8U)

/* Internal metric record with captured function name */
typedef struct ProfilerInternalMetric {
    uint32_t    fn_address;   /**< Unique function code address (key). */
    uint32_t    call_count;   /**< Total number of completed invocations. */
    uint64_t    total_cycles; /**< Accumulated execution time in CPU cycles. */
    uint32_t    min_cycles;   /**< Minimum observed execution duration in cycles. */
    uint32_t    max_cycles;   /**< Maximum observed execution duration in cycles. */
    const char* name;         /**< Captured function name literal (or NULL). */
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
    uint16_t record_count;      /**< Number of ProfilerMetric records following header. */
    uint32_t frequency;         /**< Timestamp clock frequency in Hz. */
    uint16_t dropped_functions; /**< Unique functions dropped due to capacity exhaustion. */
    uint16_t stack_overflows;   /**< Calls dropped due to shadow call stack overflow. */
    uint16_t unmatched_exits;   /**< Exits without a matching entry frame (saturating). */
    uint16_t stale_frames;      /**< Frames discarded because their exit was never seen (saturating). */
    uint16_t context_overflows; /**< Events dropped because no context slot was free (saturating). */
    uint8_t  time_mode;         /**< ProfilerTimeMode used for the recorded durations. */
    uint8_t  reserved;          /**< Reserved, always 0. */
    uint16_t probe_overhead;    /**< Calibrated scope window overhead subtracted from scope durations. */
    uint16_t nested_overhead;   /**< Calibrated untracked cost of one nested scope pair subtracted from callers. */
} ProfilerBinHeader;

/* Shadow call stack frame */
typedef struct ProfilerStackFrame {
    const void* fn_address;
    const char* name;
    uint32_t    enter_timestamp;
    uint32_t    enter_suspended;
    uint32_t    enter_probe;
} ProfilerStackFrame;

/* Context state */
typedef struct ProfilerContext {
    const void*         id;
    ProfilerStackFrame* frames;
    size_t              index;
    uint32_t            suspended_cycles;
    uint32_t            switch_out_ts;
    uint32_t            probe_cycles;
    bool                in_use;
    bool                switched_out;
} ProfilerContext;

/* Pre-allocated static storage (zero heap allocation, completely internal) */
static HashMap                s_map;
static HashMapEntry           s_map_entries[PROFILER_CAPACITY_FUNCTIONS];
static ProfilerInternalMetric s_metrics[PROFILER_CAPACITY_FUNCTIONS];
static size_t                 s_metrics_count = 0U;

static ProfilerStackFrame     s_stack_pool[PROFILER_CAPACITY_CONTEXTS * PROFILER_CAPACITY_STACK];
static ProfilerContext        s_contexts[PROFILER_CAPACITY_CONTEXTS];
static ProfilerContext        s_legacy_context;

static uint32_t               s_frequency = 0U;
static volatile bool          s_enabled = false;
static uint16_t               s_dropped_functions = 0U;
static uint16_t               s_stack_overflows = 0U;
static uint16_t               s_unmatched_exits = 0U;
static uint16_t               s_stale_frames = 0U;
static uint16_t               s_context_overflows = 0U;

/* Calibration constants */
#define PROFILER_CALIBRATION_DEPTH (2U)
static uint32_t               s_probe_overhead = 0U;
static uint32_t               s_nested_overhead = 0U;
static volatile bool          s_calibrating = false;
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
    s_calibration_owner = NULL;
}

NO_INST bool profiler_init(uint32_t frequency)
{
    if (frequency == 0U) {
        return false;
    }

    clear_state();

    if (!hashmap_init(&s_map, s_map_entries, PROFILER_CAPACITY_FUNCTIONS)) {
        return false;
    }

    s_frequency = frequency;

    s_legacy_context.frames = s_stack_pool;
    s_legacy_context.in_use = true;

    for (size_t i = 0U; i < PROFILER_CAPACITY_CONTEXTS; ++i) {
        s_contexts[i].frames = &s_stack_pool[i * PROFILER_CAPACITY_STACK];
    }

    profiler_port_init();
    return true;
}

NO_INST static void calibrate_probe(void)
{
    s_calibration_owner = profiler_port_context_id();
    (void)memset(&s_calibration_context, 0, sizeof(s_calibration_context));
    s_calibration_context.frames = s_calibration_frames;
    s_calibration_context.in_use = true;
    s_calibration_context.id = s_calibration_owner;

    s_calibration_probe = 0U;
    s_calibration_nested = 0U;
    s_calibration_min = UINT32_MAX;

    s_calibrating = true;

    for (size_t i = 0U; i < 16U; ++i) {
        profiler_scope_enter((const void*)&s_calibration_ids[0], NULL);
        profiler_scope_exit((const void*)&s_calibration_ids[0]);
    }
    s_probe_overhead = (s_calibration_min == UINT32_MAX) ? 0U : s_calibration_min;
    s_calibration_probe = s_probe_overhead;

    s_calibration_min = UINT32_MAX;
    for (size_t i = 0U; i < 16U; ++i) {
        profiler_scope_enter((const void*)&s_calibration_ids[0], NULL);
        profiler_scope_enter((const void*)&s_calibration_ids[1], NULL);
        profiler_scope_exit((const void*)&s_calibration_ids[1]);
        profiler_scope_exit((const void*)&s_calibration_ids[0]);
    }
    s_nested_overhead = (s_calibration_min == UINT32_MAX) ? 0U : s_calibration_min;

    s_calibrating = false;
    s_calibration_owner = NULL;
}

NO_INST void profiler_start(void)
{
    uint32_t state = profiler_port_enter_critical();
    if (!s_enabled) {
        reset_all_contexts(false);
        s_enabled = true;
        if (s_probe_overhead == 0U) {
            calibrate_probe();
        }
    }
    profiler_port_exit_critical(state);
}

NO_INST void profiler_stop(void)
{
    uint32_t state = profiler_port_enter_critical();
    s_enabled = false;
    profiler_port_exit_critical(state);
}

NO_INST void profiler_reset(void)
{
    uint32_t state = profiler_port_enter_critical();
    s_enabled = false;
    s_metrics_count = 0U;
    s_dropped_functions = 0U;
    s_stack_overflows = 0U;
    s_unmatched_exits = 0U;
    s_stale_frames = 0U;
    s_context_overflows = 0U;
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

NO_INST static void update_metric(const void* fn_addr, const char* name, uint32_t cycles)
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
        if ((metric->name == NULL) && (name != NULL)) {
            metric->name = name;
        }
    } else if (s_metrics_count < PROFILER_CAPACITY_FUNCTIONS) {
        ProfilerInternalMetric* metric = &s_metrics[s_metrics_count];
        metric->fn_address = (uint32_t)(uintptr_t)fn_addr;
        metric->call_count = 1U;
        metric->total_cycles = (uint64_t)cycles;
        metric->min_cycles = cycles;
        metric->max_cycles = cycles;
        metric->name = name;

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

NO_INST static void record_call(const ProfilerContext* ctx, const void* fn, const char* name, uint32_t cycles, uint32_t overhead)
{
    uint32_t net = (overhead < cycles) ? (cycles - overhead) : 0U;

    if (ctx != &s_calibration_context) {
        update_metric(fn, name, net);
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
    const char* name = ctx->frames[match_idx].name;
    uint32_t cycles = frame_cycles(ctx, &ctx->frames[match_idx], ts);
    record_call(ctx, fn, name, cycles, overhead);

    size_t discarded = ctx->index - match_pos;
    if (discarded > 0U) {
        saturating_increment(&s_stale_frames);
    }
    ctx->index = match_idx;
}

NO_INST static ProfilerContext* scope_context(void)
{
    if (s_calibrating && (profiler_port_context_id() == s_calibration_owner)) {
        return &s_calibration_context;
    }
    return current_context();
}

NO_INST __attribute__((noinline)) void profiler_scope_enter(const void* id, const char* name)
{
    if (!s_enabled || (id == NULL)) {
        return;
    }

    uint32_t state = profiler_port_enter_critical();
    uint32_t start = profiler_port_ticks();
    ProfilerContext* ctx = scope_context();

    if (ctx != NULL) {
        size_t depth = (ctx == &s_calibration_context) ? PROFILER_CALIBRATION_DEPTH : PROFILER_CAPACITY_STACK;
        ProfilerStackFrame* frame = NULL;
        if (ctx->index < depth) {
            frame = &ctx->frames[ctx->index];
            frame->fn_address = id;
            frame->name = name;
            frame->enter_suspended = ctx->suspended_cycles;
            ctx->index++;
        } else {
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

NO_INST __attribute__((noinline)) void profiler_scope_exit(const void* id)
{
    if (!s_enabled || (id == NULL)) {
        return;
    }

    uint32_t state = profiler_port_enter_critical();
    uint32_t ts = profiler_port_ticks();
    ProfilerContext* ctx = scope_context();

    if (ctx == &s_calibration_context) {
        pop_context_frame(ctx, id, ts, s_calibration_probe);
        ctx->probe_cycles += (profiler_port_ticks() - ts) + s_calibration_nested;
    } else if (ctx != NULL) {
        pop_context_frame(ctx, id, ts, s_probe_overhead);
        ctx->probe_cycles += (profiler_port_ticks() - ts) + s_nested_overhead;
    }

    profiler_port_exit_critical(state);
}

/* GCC -finstrument-functions entry/exit hooks */
NO_INST void __cyg_profile_func_enter(void* this_fn, void* call_site)
{
    (void)call_site;
    profiler_scope_enter(this_fn, NULL);
}

NO_INST void __cyg_profile_func_exit(void* this_fn, void* call_site)
{
    (void)call_site;
    profiler_scope_exit(this_fn);
}

/* Format function name or hex address up to max_len */
NO_INST static void format_fn_name(const ProfilerInternalMetric* m, char* out_buf, size_t max_len)
{
    if (m->name != NULL) {
        (void)snprintf(out_buf, max_len, "%.31s", m->name);
    } else {
        (void)snprintf(out_buf, max_len, "0x%08lX", (unsigned long)m->fn_address);
    }
}

/* Dump telemetry in BIN, CSV, or TOP format */
NO_INST bool profiler_dump(const struct Stream* stream, ProfilerDumpFormat format)
{
    if (stream == NULL) {
        return false;
    }

    uint32_t state = profiler_port_enter_critical();
    bool was_enabled = s_enabled;
    s_enabled = false;
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
        static const char top_header[] =
            "\r\n--- PROFILER RUNTIME METRICS ---\r\n"
            "Function                         Calls      Min      Max      Avg\r\n"
            "-----------------------------------------------------------------\r\n";
        (void)stream_write(stream, (const uint8_t*)top_header, sizeof(top_header) - 1U);

        char line[128];
        char fn_str[36];
        for (size_t i = 0U; i < s_metrics_count; ++i) {
            format_fn_name(&s_metrics[i], fn_str, sizeof(fn_str));
            uint32_t avg = (s_metrics[i].call_count > 0U) ?
                (uint32_t)(s_metrics[i].total_cycles / (uint64_t)s_metrics[i].call_count) : 0U;

            int len = snprintf(line, sizeof(line), "%-31s %7lu %8lu %8lu %8lu\r\n",
                               fn_str,
                               (unsigned long)s_metrics[i].call_count,
                               (unsigned long)s_metrics[i].min_cycles,
                               (unsigned long)s_metrics[i].max_cycles,
                               (unsigned long)avg);
            if (len > 0) {
                (void)stream_write(stream, (const uint8_t*)line, (size_t)len);
            }
        }
        static const char top_footer[] =
            "-----------------------------------------------------------------\r\n\r\n";
        (void)stream_write(stream, (const uint8_t*)top_footer, sizeof(top_footer) - 1U);
    }

    stream_flush(stream);

    state = profiler_port_enter_critical();
    if (was_enabled) {
        reset_all_contexts(false);
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

    /* Profiler commands MUST start with "prof-" */
    if ((len < 5U) || (strncmp(&cmd[first], "prof-", 5U) != 0)) {
        return false;
    }

    opt = &cmd[first + 5U];
    opt_len = len - 5U;

    if ((opt_len == 4U) && (strncmp(opt, "dump", 4U) == 0)) {
        profiler_stop();
        (void)profiler_dump(stream, PROFILER_DUMP_BIN);
        profiler_start();
        return true;
    } else if ((opt_len == 3U) && (strncmp(opt, "top", 3U) == 0)) {
        profiler_stop();
        (void)profiler_dump(stream, PROFILER_DUMP_TOP);
        profiler_start();
        return true;
    } else if ((opt_len == 5U) && (strncmp(opt, "reset", 5U) == 0)) {
        profiler_reset();
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
