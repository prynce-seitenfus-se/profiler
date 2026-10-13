#include "profiler_transport.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>

#include "main.h"
#include "profiler_port.h"
#include "usart.h"

#if defined(STM32G070xx)
#include "stm32g0xx_hal_uart.h"
#elif defined(STM32H533xx)
#include "stm32h5xx_hal_uart.h"
#else
#error Unsupported STM32 family for profiler UART transport
#endif

#define DEFAULT_TX_CAPACITY (256U)
#define DEFAULT_RX_CAPACITY (32U)
#define DEFAULT_TIMEOUT_MS  (1000U)

/* Internal static buffers */
static uint8_t s_internal_tx_buf[DEFAULT_TX_CAPACITY];
static uint8_t s_internal_rx_buf[DEFAULT_RX_CAPACITY];

static ProfilerTransportConfig s_config;
static Stream s_stream;
static size_t s_tx_buffered = 0U;
static volatile size_t s_rx_length = 0U;
static size_t s_rx_offset = 0U;
static volatile uint8_t s_events = PROFILER_TRANSPORT_EVENT_NONE;
static volatile ProfilerTransportStatus s_error = PROFILER_TRANSPORT_STATUS_OK;
static volatile bool s_listening = false;
static bool s_initialized = false;
static uint8_t s_rx_byte = 0U;

NO_INST static void transport_latch_error(ProfilerTransportStatus error)
{
    uint32_t state = profiler_port_enter_critical();

    if (s_error == PROFILER_TRANSPORT_STATUS_OK) {
        s_error = error;
    }
    s_events |= PROFILER_TRANSPORT_EVENT_ERROR;

    profiler_port_exit_critical(state);
}

NO_INST static bool transport_config_valid(const ProfilerTransportConfig* config)
{
    return (config != NULL) &&
           (config->device != NULL) &&
           (config->tx_buffer != NULL) && (config->tx_buffer_size > 0U) &&
           (config->tx_buffer_size <= UINT16_MAX) &&
           (config->rx_buffer != NULL) && (config->rx_buffer_size > 0U) &&
           (config->tx_timeout_ms > 0U);
}

NO_INST static bool transport_send_chunk(uint16_t length)
{
    UART_HandleTypeDef* uart = (UART_HandleTypeDef*)s_config.device;
    HAL_StatusTypeDef hal_status;

    if ((length == 0U) || (s_error != PROFILER_TRANSPORT_STATUS_OK)) {
        return (length == 0U);
    }

    hal_status = HAL_UART_Transmit(uart,
                                   s_config.tx_buffer,
                                   length,
                                   s_config.tx_timeout_ms);
    if (hal_status != HAL_OK) {
        ProfilerTransportStatus error = (hal_status == HAL_TIMEOUT) ?
                                        PROFILER_TRANSPORT_STATUS_TIMEOUT :
                                        PROFILER_TRANSPORT_STATUS_IO_ERROR;
        transport_latch_error(error);
        return false;
    }

    return true;
}

NO_INST static size_t transport_write(void* context, const uint8_t* buffer, size_t size)
{
    size_t written = 0U;

    (void)context;

    if ((buffer == NULL) || !s_initialized ||
        (s_error != PROFILER_TRANSPORT_STATUS_OK)) {
        return 0U;
    }

    while (written < size) {
        size_t available = s_config.tx_buffer_size - s_tx_buffered;
        size_t remaining = size - written;
        size_t copied = (remaining < available) ? remaining : available;

        (void)memcpy(&s_config.tx_buffer[s_tx_buffered], &buffer[written], copied);
        s_tx_buffered += copied;
        written += copied;

        if (s_tx_buffered == s_config.tx_buffer_size) {
            if (!transport_send_chunk((uint16_t)s_tx_buffered)) {
                written -= copied;
                s_tx_buffered = 0U;
                break;
            }
            s_tx_buffered = 0U;
        }
    }

    return written;
}

NO_INST static size_t transport_read(void* context, uint8_t* buffer, size_t size)
{
    size_t available;
    size_t copied;

    (void)context;

    if ((buffer == NULL) || (size == 0U) || (s_rx_offset >= s_rx_length)) {
        return 0U;
    }

    available = s_rx_length - s_rx_offset;
    copied = (size < available) ? size : available;
    (void)memcpy(buffer, &s_config.rx_buffer[s_rx_offset], copied);
    s_rx_offset += copied;

    return copied;
}

NO_INST static void transport_flush(void* context)
{
    size_t pending = s_tx_buffered;

    (void)context;
    s_tx_buffered = 0U;
    if (pending > 0U) {
        (void)transport_send_chunk((uint16_t)pending);
    }
}

NO_INST ProfilerTransportStatus profiler_transport_init_custom(const ProfilerTransportConfig* config)
{
    uint32_t state;

    if (config == NULL) {
        return PROFILER_TRANSPORT_STATUS_INVALID_PARAM;
    }

    s_config = *config;
    if ((s_config.tx_buffer == NULL) || (s_config.tx_buffer_size == 0U)) {
        s_config.tx_buffer = s_internal_tx_buf;
        s_config.tx_buffer_size = sizeof(s_internal_tx_buf);
    }
    if ((s_config.rx_buffer == NULL) || (s_config.rx_buffer_size == 0U)) {
        s_config.rx_buffer = s_internal_rx_buf;
        s_config.rx_buffer_size = sizeof(s_internal_rx_buf);
    }
    if (s_config.tx_timeout_ms == 0U) {
        s_config.tx_timeout_ms = DEFAULT_TIMEOUT_MS;
    }

    if (!transport_config_valid(&s_config)) {
        return PROFILER_TRANSPORT_STATUS_INVALID_PARAM;
    }

    s_stream = stream_init(&s_config, transport_write, transport_read, transport_flush);
    s_tx_buffered = 0U;
    s_rx_length = 0U;
    s_rx_offset = 0U;
    s_listening = false;
    s_initialized = true;

    state = profiler_port_enter_critical();
    s_events = PROFILER_TRANSPORT_EVENT_NONE;
    s_error = PROFILER_TRANSPORT_STATUS_OK;
    profiler_port_exit_critical(state);

    return profiler_transport_listen();
}

NO_INST ProfilerTransportStatus profiler_transport_init(void* device)
{
    ProfilerTransportConfig cfg;
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.device = device;
    return profiler_transport_init_custom(&cfg);
}

NO_INST const Stream* profiler_transport_stream(void)
{
    return s_initialized ? &s_stream : NULL;
}

NO_INST ProfilerTransportStatus profiler_transport_listen(void)
{
    UART_HandleTypeDef* uart;
    uint32_t state;

    if (!s_initialized) {
        return PROFILER_TRANSPORT_STATUS_NOT_READY;
    }

    state = profiler_port_enter_critical();
    if (s_listening) {
        profiler_port_exit_critical(state);
        return PROFILER_TRANSPORT_STATUS_OK;
    }
    if ((s_events != PROFILER_TRANSPORT_EVENT_NONE) ||
        (s_error != PROFILER_TRANSPORT_STATUS_OK)) {
        profiler_port_exit_critical(state);
        return PROFILER_TRANSPORT_STATUS_NOT_READY;
    }
    s_listening = true;
    s_rx_length = 0U;
    s_rx_offset = 0U;
    profiler_port_exit_critical(state);

    uart = (UART_HandleTypeDef*)s_config.device;
    if (HAL_UART_Receive_IT(uart, &s_rx_byte, 1U) != HAL_OK) {
        state = profiler_port_enter_critical();
        s_listening = false;
        profiler_port_exit_critical(state);
        transport_latch_error(PROFILER_TRANSPORT_STATUS_IO_ERROR);
        return PROFILER_TRANSPORT_STATUS_IO_ERROR;
    }

    return PROFILER_TRANSPORT_STATUS_OK;
}

NO_INST uint8_t profiler_transport_poll(size_t* out_length)
{
    uint8_t events;
    uint32_t state = profiler_port_enter_critical();

    events = s_events;
    s_events = PROFILER_TRANSPORT_EVENT_NONE;
    if (out_length != NULL) {
        *out_length = ((events & PROFILER_TRANSPORT_EVENT_FRAME) != 0U) ?
                      s_rx_length : 0U;
    }

    profiler_port_exit_critical(state);
    return events;
}

NO_INST ProfilerTransportStatus profiler_transport_status(void)
{
    ProfilerTransportStatus error;
    uint32_t state = profiler_port_enter_critical();

    error = s_error;
    s_error = PROFILER_TRANSPORT_STATUS_OK;

    profiler_port_exit_critical(state);
    return error;
}

NO_INST bool profiler_transport_process(void)
{
    size_t frame_length = 0U;
    uint8_t events;
    bool handled;

    if (!s_initialized) {
        return false;
    }

    events = profiler_transport_poll(&frame_length);
    if ((events & PROFILER_TRANSPORT_EVENT_FRAME) == 0U) {
        return false;
    }

    handled = profiler_stream_process(&s_stream);

    (void)profiler_transport_listen();
    return handled;
}

NO_INST void HAL_UART_RxCpltCallback(UART_HandleTypeDef* uart)
{
    bool rearm = false;
    uint32_t state;

    if ((uart != (UART_HandleTypeDef*)s_config.device) || !s_initialized) {
        return;
    }

    state = profiler_port_enter_critical();
    if (!s_listening) {
        profiler_port_exit_critical(state);
        return;
    }

    if (s_rx_length >= s_config.rx_buffer_size) {
        s_listening = false;
        s_events |= PROFILER_TRANSPORT_EVENT_FRAME | PROFILER_TRANSPORT_EVENT_OVERRUN;
    } else {
        s_config.rx_buffer[s_rx_length] = s_rx_byte;
        s_rx_length++;
        if (s_rx_byte == (uint8_t)'\n') {
            s_listening = false;
            s_events |= PROFILER_TRANSPORT_EVENT_FRAME;
        } else {
            rearm = true;
        }
    }
    profiler_port_exit_critical(state);

    if (rearm &&
        (HAL_UART_Receive_IT((UART_HandleTypeDef*)s_config.device, &s_rx_byte, 1U) != HAL_OK)) {
        state = profiler_port_enter_critical();
        s_listening = false;
        profiler_port_exit_critical(state);
        transport_latch_error(PROFILER_TRANSPORT_STATUS_IO_ERROR);
    }
}

NO_INST void HAL_UART_ErrorCallback(UART_HandleTypeDef* uart)
{
    if ((uart == (UART_HandleTypeDef*)s_config.device) && s_initialized) {
        uint32_t state = profiler_port_enter_critical();
        s_listening = false;
        profiler_port_exit_critical(state);
        transport_latch_error(PROFILER_TRANSPORT_STATUS_IO_ERROR);
    }
}
