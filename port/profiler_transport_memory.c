#include "profiler_transport.h"

#include <string.h>

#include "profiler_port.h"

#define DEFAULT_TX_CAPACITY (1024U)
#define DEFAULT_RX_CAPACITY (128U)

/* Internal static memory buffers */
static uint8_t s_internal_tx_buf[DEFAULT_TX_CAPACITY];
static uint8_t s_internal_rx_buf[DEFAULT_RX_CAPACITY];

static ProfilerTransportConfig s_config;
static Stream s_stream;
static size_t s_tx_length = 0U;
static size_t s_rx_length = 0U;
static size_t s_rx_offset = 0U;
static volatile uint8_t s_events = PROFILER_TRANSPORT_EVENT_NONE;
static volatile ProfilerTransportStatus s_error = PROFILER_TRANSPORT_STATUS_OK;
static bool s_initialized = false;
static bool s_listening = false;

NO_INST static void transport_latch_error(ProfilerTransportStatus error)
{
    uint32_t state = profiler_port_enter_critical();

    if (s_error == PROFILER_TRANSPORT_STATUS_OK) {
        s_error = error;
    }
    s_events |= PROFILER_TRANSPORT_EVENT_ERROR;

    profiler_port_exit_critical(state);
}

NO_INST static size_t transport_write(void* context, const uint8_t* buffer, size_t size)
{
    size_t available;
    size_t copied;

    (void)context;

    if ((buffer == NULL) && (size > 0U)) {
        transport_latch_error(PROFILER_TRANSPORT_STATUS_INVALID_PARAM);
        return 0U;
    }

    available = s_config.tx_buffer_size - s_tx_length;
    copied = (size < available) ? size : available;
    if (copied > 0U) {
        (void)memcpy(&s_config.tx_buffer[s_tx_length], buffer, copied);
        s_tx_length += copied;
    }

    if (copied != size) {
        transport_latch_error(PROFILER_TRANSPORT_STATUS_IO_ERROR);
    }

    return copied;
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
    (void)context;
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

    if ((s_config.tx_buffer == NULL) || (s_config.tx_buffer_size == 0U) ||
        (s_config.rx_buffer == NULL) || (s_config.rx_buffer_size == 0U) ||
        ((s_config.tx_timeout_ms != 0U) && (s_config.tick_ms == NULL))) {
        return PROFILER_TRANSPORT_STATUS_INVALID_PARAM;
    }

    s_stream = stream_init(&s_config, transport_write, transport_read, transport_flush);
    s_tx_length = 0U;
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
    uint32_t state;

    if (!s_initialized) {
        return PROFILER_TRANSPORT_STATUS_NOT_READY;
    }

    state = profiler_port_enter_critical();
    if (s_listening) {
        profiler_port_exit_critical(state);
        return PROFILER_TRANSPORT_STATUS_OK;
    }
    if (s_events != PROFILER_TRANSPORT_EVENT_NONE) {
        profiler_port_exit_critical(state);
        return PROFILER_TRANSPORT_STATUS_NOT_READY;
    }

    s_rx_length = 0U;
    s_rx_offset = 0U;
    s_listening = true;
    profiler_port_exit_critical(state);
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

bool profiler_transport_memory_inject_rx(const uint8_t* data, size_t length)
{
    size_t copied;
    uint32_t state;

    if ((data == NULL) && (length > 0U)) {
        transport_latch_error(PROFILER_TRANSPORT_STATUS_INVALID_PARAM);
        return false;
    }
    if (!s_initialized) {
        return false;
    }

    state = profiler_port_enter_critical();
    if (!s_listening || (s_events != PROFILER_TRANSPORT_EVENT_NONE)) {
        profiler_port_exit_critical(state);
        return false;
    }

    copied = (length < s_config.rx_buffer_size) ? length : s_config.rx_buffer_size;
    if (copied > 0U) {
        (void)memmove(s_config.rx_buffer, data, copied);
    }
    s_rx_length = copied;
    s_rx_offset = 0U;
    s_listening = false;
    s_events = PROFILER_TRANSPORT_EVENT_FRAME;
    if (copied != length) {
        s_events |= PROFILER_TRANSPORT_EVENT_OVERRUN;
        s_error = PROFILER_TRANSPORT_STATUS_IO_ERROR;
    }

    profiler_port_exit_critical(state);
    return true;
}

size_t profiler_transport_memory_output_size(void)
{
    return s_tx_length;
}
