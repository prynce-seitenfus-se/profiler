#ifndef PROFILER_TRANSPORT_H
#define PROFILER_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "profiler.h"
#include "stream.h"

/**
 * @file profiler_transport.h
 * @brief Hardware-independent deep transport contract and autonomous service for profiler I/O.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Result of a transport operation. */
typedef enum {
    PROFILER_TRANSPORT_STATUS_OK = 0,
    PROFILER_TRANSPORT_STATUS_INVALID_PARAM,
    PROFILER_TRANSPORT_STATUS_NOT_READY,
    PROFILER_TRANSPORT_STATUS_IO_ERROR,
    PROFILER_TRANSPORT_STATUS_TIMEOUT,
    PROFILER_TRANSPORT_STATUS_UNSUPPORTED
} ProfilerTransportStatus;

/** @brief Bit flags describing a latched receive event. */
#define PROFILER_TRANSPORT_EVENT_NONE    (0x00U)
#define PROFILER_TRANSPORT_EVENT_FRAME   (0x01U)
#define PROFILER_TRANSPORT_EVENT_OVERRUN (0x02U)
#define PROFILER_TRANSPORT_EVENT_ERROR   (0x04U)

/**
 * @brief Monotonic millisecond clock hook.
 *
 * @return Current free-running millisecond counter value.
 */
typedef uint32_t (*ProfilerTransportTickFn)(void);

/**
 * @brief Cooperative wait hook.
 *
 * @param milliseconds Requested wait duration.
 */
typedef void (*ProfilerTransportYieldFn)(uint32_t milliseconds);

/**
 * @brief Advanced transport configuration (optional).
 *
 * For custom buffer overrides or timing hooks. If buffers are NULL or 0,
 * backend internal static buffers (256B TX, 32B RX) are used automatically.
 */
typedef struct {
    void* device;                        /**< Backend handle, such as a UART handle. */
    uint8_t* tx_buffer;                  /**< Optional custom transmit buffer (NULL for internal). */
    size_t tx_buffer_size;               /**< Transmit buffer capacity in bytes (0 for internal 256). */
    uint8_t* rx_buffer;                  /**< Optional custom receive frame buffer (NULL for internal). */
    size_t rx_buffer_size;               /**< Receive buffer capacity in bytes (0 for internal 32). */
    uint32_t tx_timeout_ms;              /**< Per-chunk timeout (0 defaults to 1000ms). */
    ProfilerTransportTickFn tick_ms;     /**< Monotonic clock (NULL uses target HAL/port clock). */
    ProfilerTransportYieldFn yield;      /**< Cooperative wait hook (NULL permits busy/delay wait). */
} ProfilerTransportConfig;

/**
 * @brief Initializes the transport with hardware device handle and internal default buffers.
 *
 * Automatically uses internal 256B TX, 32B RX DMA-aligned buffers and target tick/yield hooks.
 * Automatically arms reception (`profiler_transport_listen`).
 *
 * @param device Target hardware handle (e.g. &huart2, or NULL for memory/null backend).
 * @return `PROFILER_TRANSPORT_STATUS_OK` on success, otherwise an error.
 */
NO_INST ProfilerTransportStatus profiler_transport_init(void* device);

/**
 * @brief Initializes the transport with custom configuration overrides.
 *
 * @param[in] config Advanced configuration.
 * @return `PROFILER_TRANSPORT_STATUS_OK` on success, otherwise an error.
 */
NO_INST ProfilerTransportStatus profiler_transport_init_custom(const ProfilerTransportConfig* config);

/**
 * @brief Autonomous command processing service loop.
 *
 * Polls the transport, delegates to profiler_stream_process() for prof-<opt> commands,
 * streams formatted responses directly, and automatically re-arms reception.
 * Returns true if a profiler command was handled during this call.
 *
 * @return true if a profiler command was processed, false otherwise.
 */
NO_INST bool profiler_transport_process(void);

/**
 * @brief Returns the initialized bidirectional Stream conduit.
 *
 * @return Stream pointer, or NULL when the backend is not initialized.
 */
NO_INST const Stream* profiler_transport_stream(void);

/**
 * @brief Arms reception of the next frame.
 *
 * Re-arming while already listening is idempotent.
 *
 * @return `PROFILER_TRANSPORT_STATUS_OK` on success, otherwise an error.
 */
NO_INST ProfilerTransportStatus profiler_transport_listen(void);

/**
 * @brief Consumes pending receive event flags.
 *
 * @param[out] out_length Optional destination for the pending frame length.
 * @return Bitwise OR of `PROFILER_TRANSPORT_EVENT_*` flags.
 */
NO_INST uint8_t profiler_transport_poll(size_t* out_length);

/**
 * @brief Reads and clears the sticky transport error.
 *
 * @return `PROFILER_TRANSPORT_STATUS_OK` when no error was latched.
 */
NO_INST ProfilerTransportStatus profiler_transport_status(void);

#ifdef __cplusplus
}
#endif

#endif /* PROFILER_TRANSPORT_H */
