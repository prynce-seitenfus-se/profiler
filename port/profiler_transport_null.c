#include "profiler_transport.h"

#include <string.h>

#include "profiler_port.h"

#define DEFAULT_TX_CAPACITY (16U)
#define DEFAULT_RX_CAPACITY (16U)

static uint8_t s_internal_tx_buf[DEFAULT_TX_CAPACITY];
static uint8_t s_internal_rx_buf[DEFAULT_RX_CAPACITY];

static ProfilerTransportConfig s_config;
static Stream s_stream;
static bool s_initialized = false;
static ProfilerTransportStatus s_error = PROFILER_TRANSPORT_STATUS_OK;

NO_INST static size_t transport_write(void* context, const uint8_t* buffer, size_t size)
{
    (void)context;

    if ((buffer == NULL) && (size > 0U)) {
        return 0U;
    }

    return size;
}

NO_INST static size_t transport_read(void* context, uint8_t* buffer, size_t size)
{
    (void)context;
    (void)buffer;
    (void)size;
    return 0U;
}

NO_INST static void transport_flush(void* context)
{
    (void)context;
}

NO_INST ProfilerTransportStatus profiler_transport_init_custom(const ProfilerTransportConfig* config)
{
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
    s_error = PROFILER_TRANSPORT_STATUS_OK;
    s_initialized = true;
    return PROFILER_TRANSPORT_STATUS_OK;
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
    return s_initialized ? PROFILER_TRANSPORT_STATUS_UNSUPPORTED :
                           PROFILER_TRANSPORT_STATUS_NOT_READY;
}

NO_INST uint8_t profiler_transport_poll(size_t* out_length)
{
    if (out_length != NULL) {
        *out_length = 0U;
    }

    return PROFILER_TRANSPORT_EVENT_NONE;
}

NO_INST ProfilerTransportStatus profiler_transport_status(void)
{
    ProfilerTransportStatus error = s_error;
    s_error = PROFILER_TRANSPORT_STATUS_OK;
    return error;
}

NO_INST bool profiler_transport_process(void)
{
    return false;
}
