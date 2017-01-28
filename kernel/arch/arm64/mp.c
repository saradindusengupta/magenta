// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2014 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <arch/mp.h>

#include <assert.h>
#include <trace.h>
#include <err.h>
#include <dev/interrupt.h>
#include <arch/ops.h>

#if WITH_DEV_INTERRUPT_ARM_GIC || WITH_DEV_INTERRUPT_ARM_GICV3
#include <dev/interrupt/arm_gic.h>
#elif PLATFORM_BCM28XX
/* bcm28xx has a weird custom interrupt controller for MP */
#include <platform/bcm28xx.h>
extern void bcm28xx_send_ipi(uint irq, uint cpu_mask);
#else
#error need other implementation of interrupt controller that can ipi
#endif

#define LOCAL_TRACE 0

#define GIC_IPI_BASE (14)

status_t arch_mp_send_ipi(mp_cpu_mask_t target, mp_ipi_t ipi)
{
    LTRACEF("target 0x%x, ipi %u\n", target, ipi);

#if WITH_DEV_INTERRUPT_ARM_GIC || WITH_DEV_INTERRUPT_ARM_GICV3
    uint gic_ipi_num = ipi + GIC_IPI_BASE;

    /* filter out targets outside of the range of cpus we care about */
    target &= ((1UL << SMP_MAX_CPUS) - 1);
    if (target != 0) {
        LTRACEF("target 0x%x, gic_ipi %u\n", target, gic_ipi_num);
        arm_gic_sgi(gic_ipi_num, ARM_GIC_SGI_FLAG_NS, target);
    }
#elif PLATFORM_BCM28XX
    /* filter out targets outside of the range of cpus we care about */
    target &= ((1UL << SMP_MAX_CPUS) - 1);
    if (target != 0) {
        bcm28xx_send_ipi(ipi, target);
    }
#endif

    return NO_ERROR;
}

static enum handler_return arm_ipi_generic_handler(void *arg)
{
    LTRACEF("cpu %u, arg %p\n", arch_curr_cpu_num(), arg);

    return mp_mbx_generic_irq();
}

static enum handler_return arm_ipi_reschedule_handler(void *arg)
{
    LTRACEF("cpu %u, arg %p\n", arch_curr_cpu_num(), arg);

    return mp_mbx_reschedule_irq();
}

void arch_mp_init_percpu(void)
{
#if WITH_DEV_INTERRUPT_ARM_GIC || WITH_DEV_INTERRUPT_ARM_GICV3
    register_int_handler(MP_IPI_GENERIC + GIC_IPI_BASE, &arm_ipi_generic_handler, 0);
    register_int_handler(MP_IPI_RESCHEDULE + GIC_IPI_BASE, &arm_ipi_reschedule_handler, 0);
    mp_set_curr_cpu_online(true);
    unmask_interrupt(MP_IPI_GENERIC+ GIC_IPI_BASE);
    unmask_interrupt(MP_IPI_RESCHEDULE+ GIC_IPI_BASE);
#elif PLATFORM_BCM28XX
    mp_set_curr_cpu_online(true);
    unmask_interrupt(INTERRUPT_ARM_LOCAL_MAILBOX0);
#endif
}

void arch_flush_state_and_halt(event_t *flush_done)
{
    PANIC_UNIMPLEMENTED;
}
