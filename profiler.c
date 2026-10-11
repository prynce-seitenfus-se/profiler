#include "profiler.h"
#include "profiler_port.h"
#include "stream.h"
#include <string.h>

/* Internal static profiler state */
static HashMap              s_map;
static ProfilerMetric*      s_metrics = NULL;
static size_t               s_metrics_capacity = 0U;
static size_t               s_metrics_count = 0U;

static ProfilerStackFrame*  s_stack_frames = NULL;
static size_t               s_stack_depth = 0U;

/* Execution contexts: legacy single stack or caller-provided per-context slots */
static ProfilerContext      s_legacy_context;
static ProfilerContext*     s_contexts = NULL;
static size_t               s_context_capacity = 0U;
static bool                 s_per_context = false;
static ProfilerContext*     s_cached_context = NULL;
static ProfilerContext*     s_running_context = NULL;
static ProfilerTimeMode     s_time_mode = PROFILER_TIME_WALL;

static uint32_t             s_frequency = 0U;
static volatile bool        s_enabled = false;
static uint16_t             s_dropped_functions = 0U;
static uint16_t             s_stack_overflows = 0U;
static uint16_t             s_unmatched_exits = 0U;
static uint16_t             s_stale_frames = 0U;
static uint16_t             s_context_overflows = 0U;

/* Scope probe calibration (see calibrate_probe) */
#define PROFILER_CALIBRATION_RUNS  (16U)
#define PROFILER_CALIBRATION_DEPTH (2U)
static uint32_t             s_probe_overhead = 0U;
static uint32_t             s_nested_overhead = 0U;
static volatile bool        s_calibrating = false;
static const void*          s_calibration_owner = NULL;
static uint32_t             s_calibration_probe = 0U;
static uint32_t             s_calibration_nested = 0U;
static uint32_t             s_calibration_min = 0U;
static ProfilerContext      s_calibration_context;
static ProfilerStackFrame   s_calibration_frames[PROFILER_CALIBRATION_DEPTH];
static const uint8_t        s_calibration_ids[PROFILER_CALIBRATION_DEPTH] = { 0U, 0U };

/* Internal polynomial calculation for IEEE 802.3 CRC-32 (0xEDB88320) */
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

/* Helper to check if a value is a non-zero power of 2 (>= 2) */
NO_INST static bool is_valid_power_of_two(size_t value)
{
    return ((value >= 2U) && ((value & (value - 1U)) == 0U));
}

NO_INST static void saturating_increment(uint16_t* counter)
{
    if (*counter < 0xFFFFU) {
        (*counter)++;
    }
}

NO_INST static void saturating_add(uint16_t* counter, size_t amount)
{
    size_t room = (size_t)(0xFFFFU - *counter);

    if (amount >= room) {
        *counter = 0xFFFFU;
    } else {
        *counter = (uint16_t)(*counter + (uint16_t)amount);
    }
}

/* Empties one context stack (new epoch); optionally releases its binding */
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

    for (size_t i = 0U; i < s_context_capacity; ++i) {
        reset_context(&s_contexts[i], unbind);
    }

    if (unbind) {
        s_cached_context = NULL;
        s_running_context = NULL;
    }
}

/* Returns the slot bound to id without binding a new one */
NO_INST static ProfilerContext* lookup_context(const void* id)
{
    if ((s_cached_context != NULL) && s_cached_context->in_use && (s_cached_context->id == id)) {
        return s_cached_context;
    }

    for (size_t i = 0U; i < s_context_capacity; ++i) {
        if (s_contexts[i].in_use && (s_contexts[i].id == id)) {
            return &s_contexts[i];
        }
    }

    return NULL;
}

NO_INST static ProfilerContext* bind_context(const void* id)
{
    for (size_t i = 0U; i < s_context_capacity; ++i) {
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

/* Returns the slot bound to id, binding a free one on first use */
NO_INST static ProfilerContext* find_context(const void* id)
{
    ProfilerContext* ctx = lookup_context(id);

    if (ctx == NULL) {
        ctx = bind_context(id);
    }
    if (ctx != NULL) {
        s_cached_context = ctx;
    }

    return ctx;
}

NO_INST static ProfilerContext* current_context(void)
{
    if (!s_per_context) {
        return &s_legacy_context;
    }

    return find_context(profiler_port_context_id());
}

NO_INST static void clear_state(void)
{
    s_enabled = false;
    s_metrics = NULL;
    s_metrics_capacity = 0U;
    s_metrics_count = 0U;
    s_stack_frames = NULL;
    s_stack_depth = 0U;
    s_contexts = NULL;
    s_context_capacity = 0U;
    s_per_context = false;
    s_cached_context = NULL;
    s_running_context = NULL;
    s_time_mode = PROFILER_TIME_WALL;
    (void)memset(&s_legacy_context, 0, sizeof(s_legacy_context));
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

NO_INST static bool is_valid_context_config(const ProfilerConfig* config)
{
    bool per_context = (config->contexts != NULL) && (config->context_capacity > 0U);

    if ((config->time_mode != PROFILER_TIME_WALL) && (config->time_mode != PROFILER_TIME_ACTIVE)) {
        return false;
    }
    if ((config->time_mode == PROFILER_TIME_ACTIVE) && !per_context) {
        return false;
    }
    if (per_context && (config->context_capacity > (SIZE_MAX / config->stack_depth))) {
        return false;
    }

    return true;
}

NO_INST static bool is_valid_config(const ProfilerConfig* config)
{
    if (config == NULL) {
        return false;
    }
    if ((config->map_entries == NULL) || (!is_valid_power_of_two(config->map_capacity))) {
        return false;
    }
    if ((config->metrics == NULL) || (config->metrics_capacity == 0U)) {
        return false;
    }
    if ((config->stack_frames == NULL) || (config->stack_depth == 0U)) {
        return false;
    }

    return is_valid_context_config(config);
}

NO_INST static void setup_contexts(const ProfilerConfig* config)
{
    s_legacy_context.frames = config->stack_frames;
    s_legacy_context.in_use = true;

    if ((config->contexts == NULL) || (config->context_capacity == 0U)) {
        return;
    }

    s_contexts = config->contexts;
    s_context_capacity = config->context_capacity;
    s_per_context = true;
    for (size_t i = 0U; i < s_context_capacity; ++i) {
        (void)memset(&s_contexts[i], 0, sizeof(ProfilerContext));
        s_contexts[i].frames = &config->stack_frames[i * config->stack_depth];
    }
}

NO_INST bool profiler_init(const ProfilerConfig* config)
{
    clear_state();

    if (!is_valid_config(config)) {
        return false;
    }

    if (!hashmap_init(&s_map, config->map_entries, config->map_capacity)) {
        return false;
    }

    s_metrics = config->metrics;
    s_metrics_capacity = config->metrics_capacity;
    s_stack_frames = config->stack_frames;
    s_stack_depth = config->stack_depth;
    s_frequency = config->frequency;
    s_time_mode = config->time_mode;
    setup_contexts(config);

    profiler_port_init();
    return true;
}

/* Minimum net duration of the outermost calibration scope over the runs */
NO_INST static uint32_t calibration_min_or_zero(void)
{
    return (s_calibration_min == UINT32_MAX) ? 0U : s_calibration_min;
}

/*
 * Times empty scopes through the public scope API on a private context:
 * 1. an empty scope gives the window overhead (end of enter to start of exit);
 * 2. a scope holding one empty nested scope, net of step 1 and of the nested
 *    scope's tracked bookkeeping, gives the untracked cost of a nested pair.
 * The minimum over several runs filters out interrupts. Only the calling
 * context uses the private context and the provisional overheads; scopes of
 * preempting contexts keep using the published values.
 */
NO_INST static void calibrate_probe(void)
{
    const void* outer = &s_calibration_ids[0];
    const void* inner = &s_calibration_ids[1];

    (void)memset(&s_calibration_context, 0, sizeof(s_calibration_context));
    s_calibration_context.frames = s_calibration_frames;
    s_calibration_context.in_use = true;
    s_calibration_probe = 0U;
    s_calibration_nested = 0U;
    s_calibration_owner = profiler_port_context_id();
    s_calibrating = true;

    s_calibration_min = UINT32_MAX;
    for (uint32_t i = 0U; i < PROFILER_CALIBRATION_RUNS; ++i) {
        profiler_scope_enter(outer);
        profiler_scope_exit(outer);
    }
    s_calibration_probe = calibration_min_or_zero();

    s_calibration_min = UINT32_MAX;
    for (uint32_t i = 0U; i < PROFILER_CALIBRATION_RUNS; ++i) {
        profiler_scope_enter(outer);
        profiler_scope_enter(inner);
        profiler_scope_exit(inner);
        profiler_scope_exit(outer);
    }
    s_calibration_nested = calibration_min_or_zero();

    uint32_t state = profiler_port_enter_critical();
    s_probe_overhead = s_calibration_probe;
    s_nested_overhead = s_calibration_nested;
    s_calibrating = false;
    profiler_port_exit_critical(state);
}

NO_INST void profiler_start(void)
{
    if ((s_metrics != NULL) && (s_stack_frames != NULL)) {
        bool started = false;
        uint32_t state = profiler_port_enter_critical();
        if (!s_enabled) {
            reset_all_contexts(false);
            s_enabled = true;
            started = true;
        }
        profiler_port_exit_critical(state);

        if (started) {
            calibrate_probe();
        }
    }
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
    hashmap_clear(&s_map);
    s_metrics_count = 0U;
    reset_all_contexts(true);
    s_dropped_functions = 0U;
    s_stack_overflows = 0U;
    s_unmatched_exits = 0U;
    s_stale_frames = 0U;
    s_context_overflows = 0U;
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

NO_INST size_t profiler_tracked_count(void)
{
    return s_metrics_count;
}

NO_INST uint16_t profiler_dropped_functions_count(void)
{
    return s_dropped_functions;
}

NO_INST uint16_t profiler_stack_overflow_count(void)
{
    return s_stack_overflows;
}

NO_INST uint16_t profiler_unmatched_exit_count(void)
{
    return s_unmatched_exits;
}

NO_INST uint16_t profiler_stale_frame_count(void)
{
    return s_stale_frames;
}

NO_INST uint16_t profiler_context_overflow_count(void)
{
    return s_context_overflows;
}

NO_INST ProfilerTimeMode profiler_time_mode(void)
{
    return s_time_mode;
}

NO_INST uint32_t profiler_probe_overhead(void)
{
    return s_probe_overhead;
}

NO_INST uint32_t profiler_nested_overhead(void)
{
    return s_nested_overhead;
}

NO_INST static uint16_t saturate_u16(uint32_t value)
{
    return (value > 0xFFFFU) ? 0xFFFFU : (uint16_t)value;
}

NO_INST bool profiler_get_metric(const void* fn_addr, ProfilerMetric* out_metric)
{
    if ((fn_addr == NULL) || (out_metric == NULL)) {
        return false;
    }

    uint32_t state = profiler_port_enter_critical();
    void* val = NULL;
    bool found = hashmap_get(&s_map, fn_addr, &val);
    if (found && (val != NULL)) {
        *out_metric = *(const ProfilerMetric*)val;
    }
    profiler_port_exit_critical(state);

    return found;
}

NO_INST static void fill_header(ProfilerBinHeader* header)
{
    (void)memset(header, 0, sizeof(*header));
    header->magic = PROFILER_BIN_MAGIC;
    header->version = PROFILER_BIN_VERSION;
    header->record_count = (uint16_t)s_metrics_count;
    header->frequency = s_frequency;
    header->dropped_functions = s_dropped_functions;
    header->stack_overflows = s_stack_overflows;
    header->unmatched_exits = s_unmatched_exits;
    header->stale_frames = s_stale_frames;
    header->context_overflows = s_context_overflows;
    header->time_mode = (uint8_t)s_time_mode;
    header->reserved = 0U;
    header->probe_overhead = saturate_u16(s_probe_overhead);
    header->nested_overhead = saturate_u16(s_nested_overhead);
}

NO_INST bool profiler_dump(const struct Stream* stream)
{
    if ((stream == NULL) || (s_metrics == NULL)) {
        return false;
    }

    uint32_t state = profiler_port_enter_critical();
    bool was_enabled = s_enabled;
    s_enabled = false;
    profiler_port_exit_critical(state);

    ProfilerBinHeader header;
    fill_header(&header);

    /* Compute CRC-32 over header and metric payload */
    uint32_t crc = calculate_crc32(0xFFFFFFFFU, (const uint8_t*)&header, sizeof(header));
    if (s_metrics_count > 0U) {
        size_t records_bytes = s_metrics_count * sizeof(ProfilerMetric);
        crc = calculate_crc32(crc, (const uint8_t*)s_metrics, records_bytes);
    }
    crc ^= 0xFFFFFFFFU;

    /* Stream 28-byte header */
    size_t written = stream_write(stream, (const uint8_t*)&header, sizeof(header));
    bool success = (written == sizeof(header));

    /* Stream contiguous metrics payload */
    if (success && (s_metrics_count > 0U)) {
        size_t records_bytes = s_metrics_count * sizeof(ProfilerMetric);
        written = stream_write(stream, (const uint8_t*)s_metrics, records_bytes);
        success = (written == records_bytes);
    }

    /* Stream 4-byte CRC-32 trailer */
    if (success) {
        written = stream_write(stream, (const uint8_t*)&crc, sizeof(crc));
        success = (written == sizeof(crc));
    }

    stream_flush(stream);

    /* Preserve cumulative metric statistics; frames in flight belong to the old epoch */
    state = profiler_port_enter_critical();
    if (was_enabled) {
        reset_all_contexts(false);
        s_enabled = true;
    }
    profiler_port_exit_critical(state);

    return success;
}

NO_INST void profiler_context_switch(const void* prev_id, const void* next_id)
{
    if (!s_enabled || !s_per_context) {
        return;
    }

    uint32_t ts = profiler_port_ticks();
    uint32_t state = profiler_port_enter_critical();

    ProfilerContext* prev = lookup_context(prev_id);
    if ((prev != NULL) && (prev_id != next_id)) {
        prev->switch_out_ts = ts;
        prev->switched_out = true;
    }

    /* The incoming context resumes after this bookkeeping: read the clock again
     * so that the hook's own cost is charged to neither context */
    ProfilerContext* next = find_context(next_id);
    if ((next != NULL) && next->switched_out) {
        next->suspended_cycles += profiler_port_ticks() - next->switch_out_ts;
        next->switched_out = false;
    }
    s_running_context = next;

    profiler_port_exit_critical(state);
}

NO_INST void profiler_context_release(const void* id)
{
    if (!s_per_context) {
        return;
    }

    uint32_t state = profiler_port_enter_critical();
    ProfilerContext* ctx = lookup_context(id);
    if (ctx != NULL) {
        reset_context(ctx, true);
        if (s_cached_context == ctx) {
            s_cached_context = NULL;
        }
        if (s_running_context == ctx) {
            s_running_context = NULL;
        }
    }
    profiler_port_exit_critical(state);
}

NO_INST static void update_metric(const void* fn_addr, uint32_t cycles)
{
    void** slot = hashmap_get_ref(&s_map, fn_addr);

    if (slot != NULL) {
        ProfilerMetric* metric = (ProfilerMetric*)(*slot);
        metric->call_count++;
        metric->total_cycles += (uint64_t)cycles;
        if (cycles < metric->min_cycles) {
            metric->min_cycles = cycles;
        }
        if (cycles > metric->max_cycles) {
            metric->max_cycles = cycles;
        }
    } else if (s_metrics_count < s_metrics_capacity) {
        ProfilerMetric* metric = &s_metrics[s_metrics_count];
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

/* Returns the index + 1 of the most recent frame of fn, or 0 when absent */
NO_INST static size_t find_frame(const ProfilerContext* ctx, const void* fn)
{
    for (size_t i = ctx->index; i > 0U; --i) {
        if (ctx->frames[i - 1U].fn_address == fn) {
            return i;
        }
    }

    return 0U;
}

/* Duration of a frame without the bookkeeping of nested scopes; in active mode
 * the switched-out time is excluded too */
NO_INST static uint32_t frame_cycles(const ProfilerContext* ctx, const ProfilerStackFrame* frame, uint32_t ts)
{
    uint32_t cycles = ts - frame->enter_timestamp;
    uint32_t probe = ctx->probe_cycles - frame->enter_probe;

    cycles = (probe < cycles) ? (cycles - probe) : 0U;

    if (s_time_mode == PROFILER_TIME_ACTIVE) {
        uint32_t suspended = ctx->suspended_cycles - frame->enter_suspended;
        cycles = (suspended < cycles) ? (cycles - suspended) : 0U;
    }

    return cycles;
}

/* Active mode: time spent in the outermost instrumented ISR frame is charged
 * as suspended time to the task context it preempted */
NO_INST static void charge_isr_time(const ProfilerContext* ctx, size_t idx, uint32_t cycles)
{
    if ((s_time_mode == PROFILER_TIME_ACTIVE) && (idx == 0U) && (ctx->id == PROFILER_CONTEXT_ISR) &&
        (s_running_context != NULL) && (s_running_context != ctx)) {
        s_running_context->suspended_cycles += cycles;
    }
}

/* Records a completed call; scopes pass their calibrated window overhead */
NO_INST static void record_call(const ProfilerContext* ctx, const void* fn, uint32_t cycles, uint32_t overhead)
{
    uint32_t net = (overhead < cycles) ? (cycles - overhead) : 0U;

    if (ctx != &s_calibration_context) {
        update_metric(fn, net);
    } else if ((fn == (const void*)&s_calibration_ids[0]) && (net < s_calibration_min)) {
        s_calibration_min = net;
    } else {
        /* Nested calibration scope: not recorded */
    }
}

/* Per-context exit: frames above the match are callees whose exit was lost */
NO_INST static void pop_context_frame(ProfilerContext* ctx, const void* fn, uint32_t ts, uint32_t overhead)
{
    size_t found = find_frame(ctx, fn);

    if (found == 0U) {
        saturating_increment(&s_unmatched_exits);
        return;
    }

    size_t idx = found - 1U;
    uint32_t cycles = frame_cycles(ctx, &ctx->frames[idx], ts);
    record_call(ctx, fn, cycles, overhead);
    charge_isr_time(ctx, idx, cycles);
    saturating_add(&s_stale_frames, ctx->index - found);
    ctx->index = idx;
}

/* Legacy exit: one stack shared by all contexts, so frames above the match may
 * belong to other contexts and are preserved by shifting them down */
NO_INST static void pop_legacy_frame(ProfilerContext* ctx, const void* fn, uint32_t ts, uint32_t overhead)
{
    size_t found = find_frame(ctx, fn);

    if (found == 0U) {
        saturating_increment(&s_unmatched_exits);
        return;
    }

    size_t idx = found - 1U;
    record_call(ctx, fn, frame_cycles(ctx, &ctx->frames[idx], ts), overhead);
    for (size_t j = idx; (j + 1U) < ctx->index; ++j) {
        ctx->frames[j] = ctx->frames[j + 1U];
    }
    ctx->index--;
}

NO_INST void __cyg_profile_func_enter(void* this_fn, void* call_site)
{
    (void)call_site;

    if (!s_enabled || (this_fn == NULL)) {
        return;
    }

    uint32_t ts = profiler_port_ticks();
    uint32_t state = profiler_port_enter_critical();
    ProfilerContext* ctx = current_context();

    if (ctx != NULL) {
        if (ctx->index < s_stack_depth) {
            ProfilerStackFrame* frame = &ctx->frames[ctx->index];
            frame->fn_address = this_fn;
            frame->enter_timestamp = ts;
            frame->enter_suspended = ctx->suspended_cycles;
            frame->enter_probe = ctx->probe_cycles;
            ctx->index++;
        } else {
            saturating_increment(&s_stack_overflows);
        }
    }

    profiler_port_exit_critical(state);
}

NO_INST void __cyg_profile_func_exit(void* this_fn, void* call_site)
{
    (void)call_site;

    if (!s_enabled || (this_fn == NULL)) {
        return;
    }

    uint32_t ts = profiler_port_ticks();
    uint32_t state = profiler_port_enter_critical();
    ProfilerContext* ctx = current_context();

    if (ctx != NULL) {
        if (s_per_context) {
            pop_context_frame(ctx, this_fn, ts, 0U);
        } else {
            pop_legacy_frame(ctx, this_fn, ts, 0U);
        }
    }

    profiler_port_exit_critical(state);
}

/* Scope probes. Timestamps are read inside the critical section so that no
 * context switch or interrupt falls between them; the time between the two
 * reads of each probe is tracked bookkeeping, excluded from enclosing frames */
NO_INST static ProfilerContext* scope_context(void)
{
    if (s_calibrating && (profiler_port_context_id() == s_calibration_owner)) {
        return &s_calibration_context;
    }

    return current_context();
}

NO_INST __attribute__((noinline)) void profiler_scope_enter(const void* id)
{
    if (!s_enabled || (id == NULL)) {
        return;
    }

    uint32_t state = profiler_port_enter_critical();
    uint32_t start = profiler_port_ticks();
    ProfilerContext* ctx = scope_context();

    if (ctx != NULL) {
        size_t depth = (ctx == &s_calibration_context) ? PROFILER_CALIBRATION_DEPTH : s_stack_depth;
        ProfilerStackFrame* frame = NULL;
        if (ctx->index < depth) {
            frame = &ctx->frames[ctx->index];
            frame->fn_address = id;
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
        if (s_per_context) {
            pop_context_frame(ctx, id, ts, s_probe_overhead);
        } else {
            pop_legacy_frame(ctx, id, ts, s_probe_overhead);
        }
        ctx->probe_cycles += (profiler_port_ticks() - ts) + s_nested_overhead;
    } else {
        /* No context slot available: nothing to close */
    }

    profiler_port_exit_critical(state);
}
