#include "profiler_port.h"

#include <stddef.h>

/* Default execution context hook: a single context for every event.
 * Applications or RTOS ports override it with a strong definition. */
__attribute__((weak)) NO_INST const void* profiler_port_context_id(void)
{
    return NULL;
}
