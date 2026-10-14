#ifndef PROFILER_H
#define PROFILER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Forward declaration of Stream */
struct Stream;

#ifndef NO_INST
#define NO_INST __attribute__((no_instrument_function))
#endif

#define PROFILER_BIN_MAGIC    (0x464F5250U)
#define PROFILER_BIN_VERSION  (0x0004U)

/**
 * @brief Reserved execution context identifier for handler (interrupt) mode.
 */
#define PROFILER_CONTEXT_ISR  ((const void*)1U)

#ifdef PROFILER_SCOPES_ENABLED
#error "Profiler scopes were removed; compile application code with -finstrument-functions."
#endif

/**
 * @brief Output data representation formats for profiler telemetry.
 */
typedef enum ProfilerDumpFormat {
    PROFILER_DUMP_BIN = 0, /**< Compact PROF-BIN v4 binary format with CRC32 */
    PROFILER_DUMP_CSV = 1, /**< ASCII CSV: function,calls,min,max,avg */
    PROFILER_DUMP_TOP = 2  /**< ASCII terminal table view with function addresses */
} ProfilerDumpFormat;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the profiler engine and optional transport backend.
 *
 * Allocates all hashmap entries, metrics, shadow stacks, and context tables
 * internally within the library (capacity: 128 functions, 16 stack depth, 8 contexts).
 * If a non-NULL transport_device is provided (e.g. &huart2), the selected transport
 * backend is initialized automatically. Pass NULL for headless or standalone operation.
 *
 * @param frequency        Clock frequency in Hz of the timestamp counter.
 * @param transport_device Optional device handle for the transport backend (e.g. &huart2),
 *                         or NULL if no transport is desired / standalone operation.
 * @return true if initialized successfully, false on invalid parameter or transport failure.
 */
NO_INST bool profiler_init(uint32_t frequency, void* transport_device);

/**
 * @brief Processes incoming profiler commands over the active transport.
 *
 * Safe to call from the application main loop. If no transport was initialized,
 * this function is a safe no-op returning false.
 *
 * @return true if a profiler command was processed, false otherwise.
 */
NO_INST bool profiler_process(void);

/**
 * @brief Starts profiling by enabling instrumentation hooks.
 *
 * First-start calibration masks interrupts for one sample at a time, permitting
 * interrupts/context switches between samples. Normal measurements remain disabled
 * until calibration completes. A concurrent start is ignored; stop/reset cancels
 * pending calibration. An already-masked caller retains its original mask.
 */
NO_INST void profiler_start(void);

/**
 * @brief Stops profiling by disabling instrumentation hooks.
 */
NO_INST void profiler_stop(void);

/**
 * @brief Resets all accumulated metric statistics, shadow stacks, and contexts.
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
 * @brief Streams profiler telemetry in the requested format over an abstract Stream.
 *
 * @param[in] stream Output stream conduit.
 * @param[in] format Data representation format (BIN, CSV, or TOP).
 * @return true on success, false on I/O error or invalid parameter.
 */
NO_INST bool profiler_dump(const struct Stream* stream, ProfilerDumpFormat format);

/**
 * @brief Compiler-generated function entry hook; do not call from application code.
 * @param[in] this_fn Function address supplied by -finstrument-functions.
 * @param[in] call_site Caller address (currently unused).
 */
NO_INST void __cyg_profile_func_enter(void* this_fn, void* call_site);

/**
 * @brief Compiler-generated function exit hook; do not call from application code.
 * @param[in] this_fn Function address supplied by -finstrument-functions.
 * @param[in] call_site Caller address (currently unused).
 */
NO_INST void __cyg_profile_func_exit(void* this_fn, void* call_site);

/**
 * @brief Notifies the profiler of an RTOS context switch.
 *
 * @param[in] prev_id Identifier of the context being switched out (may be NULL).
 * @param[in] next_id Identifier of the context being switched in.
 */
NO_INST void profiler_context_switch(const void* prev_id, const void* next_id);

/**
 * @brief Releases the context slot bound to an identifier.
 *
 * @param[in] id Context identifier to release.
 */
NO_INST void profiler_context_release(const void* id);

/**
 * @brief Executes a profiler command string if it begins with "prof-".
 *
 * Supported commands:
 *   - prof-dump   : Dumps binary profiler telemetry (PROF-BIN)
 *   - prof-top    : Dumps formatted ASCII execution table
 *   - prof-reset  : Resets accumulated statistics
 *   - prof-status : Prints profiler runtime status and clock frequency
 *   - prof-help   : Prints supported profiler commands
 *
 * Non-"prof-" commands are rejected and return false.
 *
 * @param[in] cmd    Null-terminated command string. Must begin with "prof-".
 * @param[in] stream Output stream conduit for telemetry/response.
 * @return true if command started with "prof-" and was executed, false otherwise.
 */
NO_INST bool profiler_cmd_execute(const char* cmd, const struct Stream* stream);

/**
 * @brief Processes incoming profiler commands over an arbitrary Stream.
 *
 * Reads incoming bytes from the stream, parses lines, and passes any command
 * beginning with "prof-" to profiler_cmd_execute(). Non-profiler commands are ignored.
 *
 * @param[in] stream Bidirectional stream conduit.
 * @return true if a profiler command was processed, false otherwise.
 */
NO_INST bool profiler_stream_process(const struct Stream* stream);

#ifdef __cplusplus
}
#endif

#endif /* PROFILER_H */
