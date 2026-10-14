#ifndef PROFILER_PORT_H
#define PROFILER_PORT_H

#include <stdint.h>
#include <stdbool.h>

#include "profiler.h"

/**
 * @file profiler_port.h
 * @brief Target port contract implemented by the platform port backend.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the hardware timestamp counter peripheral.
 */
NO_INST void profiler_port_init(void);

/**
 * @brief Reads the free-running hardware timestamp counter.
 *
 * @return Current timestamp tick value.
 */
NO_INST uint32_t profiler_port_ticks(void);

/**
 * @brief Enters a critical section, disabling interrupts.
 *
 * @return Target-specific previous interrupt/state mask to restore on exit.
 */
NO_INST uint32_t profiler_port_enter_critical(void);

/**
 * @brief Exits a critical section, restoring interrupt state.
 *
 * @param[in] state Previous interrupt state returned by profiler_port_enter_critical().
 */
NO_INST void profiler_port_exit_critical(uint32_t state);

/**
 * @brief Returns an opaque identifier of the current execution context.
 *
 * RTOS integrations return the running task handle, or PROFILER_CONTEXT_ISR in
 * handler mode. NULL selects the independent bare-metal context. The weak
 * default (profiler_port_context_default.c) returns NULL for every event.
 *
 * @return Context identifier.
 */
NO_INST const void* profiler_port_context_id(void);

/* -------------------------------------------------------------------------
 * Hardware Adapter Hooks for 16-bit and 2x16-bit Timer Ports
 * (Implemented by the application when using profiler_port_16bit_it or
 *  profiler_port_2x16bit)
 * ------------------------------------------------------------------------- */

/**
 * @brief Application hook to read the low 16-bit hardware timer counter register.
 *
 * Required by profiler_port_16bit_it and profiler_port_2x16bit.
 *
 * @return 16-bit counter register value.
 */
NO_INST uint16_t profiler_port_hardware_timer16_read_low(void);

/**
 * @brief Application hook to read the high 16-bit hardware timer counter register.
 *
 * Required by profiler_port_2x16bit only.
 *
 * @return 16-bit counter register value of the slave timer.
 */
NO_INST uint16_t profiler_port_hardware_timer16_read_high(void);

/**
 * @brief Optional application hook to initialize timer hardware peripherals.
 *
 * Called by profiler_port_init() in profiler_port_16bit_it and profiler_port_2x16bit.
 */
NO_INST void profiler_port_hardware_timer16_init(void);

/**
 * @brief Application hook reporting a timer overflow not yet serviced by its ISR.
 *
 * Required by profiler_port_16bit_it. Return the hardware update/overflow flag
 * (e.g. TIMx_SR.UIF) without clearing it. It lets profiler_port_ticks() stay
 * monotonic when called with interrupts masked (context-switch hook in PendSV).
 * The masked period must be shorter than half a timer period (32768 ticks).
 *
 * @return true if the counter wrapped and the overflow ISR is still pending.
 */
NO_INST bool profiler_port_hardware_timer16_overflow_pending(void);

/**
 * @brief Interrupt service routine hook to notify profiler_port_16bit_it of a timer overflow.
 *
 * Must be invoked by the application from the timer update/overflow ISR.
 */
NO_INST void profiler_port_16bit_it_overflow_isr(void);

#ifdef __cplusplus
}
#endif

#endif /* PROFILER_PORT_H */
