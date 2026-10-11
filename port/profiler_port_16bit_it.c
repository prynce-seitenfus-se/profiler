#include "profiler_port.h"

static volatile uint16_t s_timer_high_word = 0U;

NO_INST void profiler_port_init(void)
{
    s_timer_high_word = 0U;
    profiler_port_hardware_timer16_init();
}

NO_INST uint32_t profiler_port_ticks(void)
{
    uint16_t high_before = s_timer_high_word;
    uint16_t low = profiler_port_hardware_timer16_read_low();
    bool pending = profiler_port_hardware_timer16_overflow_pending();
    uint16_t high_after = s_timer_high_word;

    if (high_before != high_after) {
        /* The overflow ISR ran while sampling: re-read the counter */
        low = profiler_port_hardware_timer16_read_low();
        high_before = high_after;
    } else if (pending && (low < 0x8000U)) {
        /*
         * The counter wrapped but the overflow ISR has not run yet (interrupts
         * masked, e.g. inside PendSV): account for the pending overflow. A low
         * value in the upper half was sampled before the wrap.
         */
        high_before++;
    } else {
        /* No overflow around the sample */
    }

    return ((uint32_t)high_before << 16U) | (uint32_t)low;
}

NO_INST void profiler_port_16bit_it_overflow_isr(void)
{
    s_timer_high_word++;
}

#if defined(__arm__) || defined(__thumb__)

NO_INST uint32_t profiler_port_enter_critical(void)
{
    uint32_t prev_primask = 0U;
    __asm__ volatile (
        "mrs %0, primask\n"
        "cpsid i\n"
        : "=r" (prev_primask) :: "memory"
    );
    return prev_primask;
}

NO_INST void profiler_port_exit_critical(uint32_t state)
{
    __asm__ volatile (
        "msr primask, %0\n"
        :: "r" (state) : "memory"
    );
}

#elif defined(__riscv)

NO_INST uint32_t profiler_port_enter_critical(void)
{
    uint32_t prev_mstatus;
    __asm__ volatile ("csrci mstatus, 8\n"
                      "csrr %0, mstatus"
                      : "=r" (prev_mstatus) :: "memory");
    return prev_mstatus;
}

NO_INST void profiler_port_exit_critical(uint32_t state)
{
    if ((state & (1U << 3U)) != 0U) {
        __asm__ volatile ("csrsi mstatus, 8" ::: "memory");
    }
}

#else

NO_INST uint32_t profiler_port_enter_critical(void)
{
    return 0U;
}

NO_INST void profiler_port_exit_critical(uint32_t state)
{
    (void)state;
}

#endif
