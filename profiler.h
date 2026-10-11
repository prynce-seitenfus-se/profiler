#ifndef PROFILER_H
#define PROFILER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "hashmap.h"

/* Forward declaration of Stream */
struct Stream;

#ifndef NO_INST
#define NO_INST __attribute__((no_instrument_function))
#endif

#define PROFILER_BIN_MAGIC    (0x464F5250U)
#define PROFILER_BIN_VERSION  (0x0004U)

/**
 * @brief Reserved execution context identifier for handler (interrupt) mode.
 *
 * All interrupt handlers share one context because ISR nesting is strictly LIFO.
 */
#define PROFILER_CONTEXT_ISR  ((const void*)1U)

/**
 * @brief Converts a function designator into a scope identifier (its code address).
 */
#define PROFILER_FN_ID(fn)    ((const void*)(uintptr_t)(fn))

/**
 * @brief Measures one call site with an explicit scope (no compiler instrumentation).
 *
 * Expands to profiler_scope_enter(fn) / call / profiler_scope_exit(fn) when
 * PROFILER_SCOPES_ENABLED is defined, and to the bare call otherwise. Example:
 * PROFILER_SCOPE(sertos_queue_send, status = sertos_queue_send(q, &item, 0U));
 *
 * @param fn   Measured function; its address keys the metric.
 * @param call Statement performing the call.
 */
#ifdef PROFILER_SCOPES_ENABLED
#define PROFILER_SCOPE(fn, call)                      \
    do {                                              \
        profiler_scope_enter(PROFILER_FN_ID(fn));     \
        call;                                         \
        profiler_scope_exit(PROFILER_FN_ID(fn));      \
    } while (0)
#else
#define PROFILER_SCOPE(fn, call) \
    do {                         \
        call;                    \
    } while (0)
#endif

/**
 * @brief Duration accounting mode.
 */
typedef enum ProfilerTimeMode {
    PROFILER_TIME_WALL = 0,  /**< Elapsed time between entry and exit (default). */
    PROFILER_TIME_ACTIVE = 1 /**< Elapsed time minus the time the owning context was switched out. */
} ProfilerTimeMode;

/**
 * @brief Aggregated runtime performance metrics for a unique instrumented function.
 * Total size: 24 bytes (naturally 8-byte aligned on 32-bit and 64-bit architectures).
 */
typedef struct ProfilerMetric {
    uint32_t fn_address;   /**< Unique function code address (key). */
    uint32_t call_count;   /**< Total number of completed invocations. */
    uint64_t total_cycles; /**< Accumulated execution time in CPU cycles. */
    uint32_t min_cycles;   /**< Minimum observed execution duration in cycles. */
    uint32_t max_cycles;   /**< Maximum observed execution duration in cycles. */
} ProfilerMetric;

/**
 * @brief Shadow call stack activation frame for active in-flight invocations.
 */
typedef struct ProfilerStackFrame {
    const void* fn_address;      /**< Address of the executing function. */
    uint32_t    enter_timestamp; /**< Timestamp snapshot taken on entry. */
    uint32_t    enter_suspended; /**< Owning context suspended-cycle counter at entry (active mode). */
    uint32_t    enter_probe;     /**< Owning context probe-cycle counter at entry (nested scopes). */
} ProfilerStackFrame;

/**
 * @brief Per-execution-context shadow call stack slot (caller-allocated).
 *
 * The profiler binds one slot to each execution context identifier reported by
 * profiler_port_context_id(). Fields are owned by the profiler after init.
 */
typedef struct ProfilerContext {
    const void*         id;               /**< Owner key (task handle, PROFILER_CONTEXT_ISR, or NULL). */
    ProfilerStackFrame* frames;           /**< Slice of the shared frame pool (stack_depth frames). */
    size_t              index;            /**< Current nesting depth. */
    uint32_t            suspended_cycles; /**< Accumulated switched-out (or preempted) time. */
    uint32_t            switch_out_ts;    /**< Timestamp of the last switch-out. */
    uint32_t            probe_cycles;     /**< Accumulated scope bookkeeping time (excluded from callers). */
    bool                in_use;           /**< True while the slot is bound to an id. */
    bool                switched_out;     /**< True between switch-out and switch-in. */
} ProfilerContext;

/**
 * @brief Binary protocol header for streaming dumps (PROF-BIN v4, 28 bytes).
 */
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
    uint16_t probe_overhead;    /**< Calibrated scope window overhead subtracted from scope durations (saturating). */
    uint16_t nested_overhead;   /**< Calibrated untracked cost of one nested scope pair subtracted from callers (saturating). */
} ProfilerBinHeader;

/**
 * @brief Application configuration provided to profiler_init().
 *
 * Legacy single-stack mode: contexts == NULL or context_capacity == 0. All events
 * share stack_frames (stack_depth frames) and profiler_port_context_id() is not used.
 *
 * Per-context mode: stack_frames must hold stack_depth * context_capacity frames;
 * each bound context owns a stack_depth slice.
 */
typedef struct ProfilerConfig {
    uint32_t            frequency;        /**< Timestamp clock frequency in Hz. */
    HashMapEntry*       map_entries;      /**< Static array of HashMapEntry (power of 2). */
    size_t              map_capacity;     /**< Total slot capacity of map_entries (>= 2). */
    ProfilerMetric*     metrics;          /**< Caller-allocated array of ProfilerMetric. */
    size_t              metrics_capacity; /**< Capacity of metrics array (must be >= 75% of map_capacity). */
    ProfilerStackFrame* stack_frames;     /**< Caller-allocated shadow call stack frame pool. */
    size_t              stack_depth;      /**< Maximum nesting depth per context. */
    ProfilerContext*    contexts;         /**< Caller-allocated context slots (NULL for legacy mode). */
    size_t              context_capacity; /**< Number of context slots (0 for legacy mode). */
    ProfilerTimeMode    time_mode;        /**< Duration accounting mode (ACTIVE requires contexts). */
} ProfilerConfig;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the profiler with caller-supplied buffers and configuration.
 *
 * Validates parameters, initializes the internal HashMap container over map_entries,
 * and resets all internal metric accumulators and shadow stack state.
 *
 * @param[in] config Pointer to the configuration structure.
 * @return true if initialized successfully, false on invalid parameters.
 */
NO_INST bool profiler_init(const ProfilerConfig* config);

/**
 * @brief Starts profiling by enabling instrumentation hooks.
 *
 * On a disabled-to-enabled transition every shadow stack is emptied (new epoch),
 * so exits of calls entered before the transition are counted as unmatched
 * instead of being paired with unrelated frames. No-op if already enabled.
 */
NO_INST void profiler_start(void);

/**
 * @brief Stops profiling by disabling instrumentation hooks.
 */
NO_INST void profiler_stop(void);

/**
 * @brief Resets all accumulated metrics, shadow call stacks, context bindings, diagnostics, and table entries.
 */
NO_INST void profiler_reset(void);

/**
 * @brief Queries whether profiling is currently enabled.
 *
 * @return true if enabled, false otherwise.
 */
NO_INST bool profiler_enabled(void);

/**
 * @brief Returns the configured timestamp frequency in Hz.
 *
 * @return Frequency value supplied in profiler_init().
 */
NO_INST uint32_t profiler_frequency(void);

/**
 * @brief Returns the number of unique functions currently tracked in the hash map.
 *
 * @return Count of occupied metric entries.
 */
NO_INST size_t profiler_tracked_count(void);

/**
 * @brief Returns the number of unique functions dropped due to full table capacity.
 *
 * @return Count of dropped unique functions.
 */
NO_INST uint16_t profiler_dropped_functions_count(void);

/**
 * @brief Returns the count of calls dropped due to shadow stack overflow.
 *
 * @return Count of dropped shadow stack entries.
 */
NO_INST uint16_t profiler_stack_overflow_count(void);

/**
 * @brief Returns the count of exits that found no matching entry frame.
 *
 * Expected right after profiler_start() or profiler_dump() for calls that were
 * in flight while hooks were disabled.
 *
 * @return Saturating count of unmatched exits.
 */
NO_INST uint16_t profiler_unmatched_exit_count(void);

/**
 * @brief Returns the count of frames discarded because their exit was never seen.
 *
 * @return Saturating count of stale frames (per-context mode only).
 */
NO_INST uint16_t profiler_stale_frame_count(void);

/**
 * @brief Returns the count of events dropped because no context slot was free.
 *
 * @return Saturating count of context overflows.
 */
NO_INST uint16_t profiler_context_overflow_count(void);

/**
 * @brief Returns the configured duration accounting mode.
 *
 * @return ProfilerTimeMode supplied in profiler_init().
 */
NO_INST ProfilerTimeMode profiler_time_mode(void);

/**
 * @brief Notifies the profiler of an RTOS context switch.
 *
 * Must be called from the scheduler switch path with interrupts masked and must
 * not be instrumented. Required for PROFILER_TIME_ACTIVE; in PROFILER_TIME_WALL
 * it only warms the context lookup cache. No-op in legacy mode or when disabled.
 *
 * @param[in] prev_id Identifier of the context being switched out (may be NULL).
 * @param[in] next_id Identifier of the context being switched in.
 */
NO_INST void profiler_context_switch(const void* prev_id, const void* next_id);

/**
 * @brief Releases the context slot bound to an identifier (e.g. a deleted task).
 *
 * No-op if the identifier is not bound.
 *
 * @param[in] id Context identifier to release.
 */
NO_INST void profiler_context_release(const void* id);

/**
 * @brief Retrieves a copy of the metrics associated with a function address.
 *
 * @param[in]  fn_addr    Function address to look up.
 * @param[out] out_metric Destination pointer to copy the metrics into.
 * @return true if function was found, false otherwise.
 */
NO_INST bool profiler_get_metric(const void* fn_addr, ProfilerMetric* out_metric);

/**
 * @brief Opens an explicit measurement scope (use PROFILER_SCOPE()).
 *
 * Alternative to -finstrument-functions: only the wrapped call sites are
 * measured, so uninstrumented code runs unchanged inside the scope. Scopes
 * share the shadow stacks, time modes and metrics with instrumented functions.
 * Recorded durations are net of the calibrated probe overhead, and the
 * bookkeeping of nested scopes is excluded from enclosing scopes. Do not open
 * a scope for a function that is itself compiled with -finstrument-functions.
 *
 * @param[in] id Scope identifier, normally PROFILER_FN_ID(function).
 */
NO_INST void profiler_scope_enter(const void* id);

/**
 * @brief Closes the scope opened by profiler_scope_enter() with the same id.
 *
 * @param[in] id Scope identifier passed to profiler_scope_enter().
 */
NO_INST void profiler_scope_exit(const void* id);

/**
 * @brief Returns the calibrated scope window overhead in timestamp ticks.
 *
 * Measured by profiler_start() as the minimum duration of an empty scope; it is
 * subtracted from every scope duration.
 *
 * @return Probe overhead (0 before the first profiler_start()).
 */
NO_INST uint32_t profiler_probe_overhead(void);

/**
 * @brief Returns the calibrated untracked cost of one nested scope pair.
 *
 * Measured by profiler_start(); it is subtracted from the enclosing scope for
 * each nested scope, together with the nested scope's tracked bookkeeping time.
 *
 * @return Nested scope overhead (0 before the first profiler_start()).
 */
NO_INST uint32_t profiler_nested_overhead(void);

/**
 * @brief Streams packed binary profiler statistics over an abstract Stream.
 *
 * Emits the 28-byte ProfilerBinHeader, streams all ProfilerMetric records directly
 * using zero-copy transfers, and finishes with a 32-bit CRC checksum.
 * Does NOT reset accumulated statistics (cumulative telemetry across calls).
 *
 * @param[in] stream Pointer to the target Stream instance.
 * @return true on successful transmission, false on stream error or invalid argument.
 */
NO_INST bool profiler_dump(const struct Stream* stream);

#ifdef __cplusplus
}
#endif

#endif /* PROFILER_H */
