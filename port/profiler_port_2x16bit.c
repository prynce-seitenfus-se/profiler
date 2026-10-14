#include "profiler_port.h"

NO_INST void profiler_port_init(void)
{
    profiler_port_hardware_timer16_init();
}

NO_INST uint32_t profiler_port_ticks(void)
{
    uint32_t high_before = (uint32_t)profiler_port_hardware_timer16_read_high();
    uint32_t low = (uint32_t)profiler_port_hardware_timer16_read_low();
    uint32_t high_after = (uint32_t)profiler_port_hardware_timer16_read_high();

    /* Branchless wrap selection to eliminate execution-time jitter after sampling low. */
    uint32_t low_msb_mask = 0U - (low >> 15U);
    uint32_t high = (high_before & low_msb_mask) | (high_after & ~low_msb_mask);

    return (high << 16U) | low;
}

NO_INST void profiler_port_16bit_it_overflow_isr(void)
{
    /* No-op when using hardware cascaded 2x16-bit timers */
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
