/*
 * VIA NVRAM
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
 * Copyright (c) 2025 Tisenu100
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef HW_RTC_VIA_NVRAM_H
#define HW_RTC_VIA_NVRAM_H

#include "qapi/qapi-types-machine.h"
#include "qemu/queue.h"
#include "qemu/timer.h"
#include "hw/isa/isa.h"
#include "qom/object.h"

#define TYPE_VIA_NVRAM "via-nvram"
OBJECT_DECLARE_SIMPLE_TYPE(VIANVRAMState, VIA_NVRAM)

struct VIANVRAMState {
    ISADevice parent_obj;

    MemoryRegion io;
    MemoryRegion extended_io;
    MemoryRegion coalesced_io;
    uint8_t cmos_data[256];
    uint8_t cmos_index;
    uint8_t isairq;
    uint16_t io_base;
    uint16_t extended_io_base;
    int32_t base_year;
    uint64_t base_rtc;
    uint64_t last_update;
    int64_t offset;
    qemu_irq irq;
    int it_shift;
    /* periodic timer */
    QEMUTimer *periodic_timer;
    int64_t next_periodic_time;
    /* update-ended timer */
    QEMUTimer *update_timer;
    uint64_t next_alarm_time;
    uint16_t irq_reinject_on_ack_count;
    uint32_t irq_coalesced;
    uint32_t period;
    QEMUTimer *coalesced_timer;
    Notifier clock_reset_notifier;
    LostTickPolicy lost_tick_policy;
    Notifier suspend_notifier;
    QLIST_ENTRY(VIANVRAMState) link;
};

#define RTC_ISA_IRQ 8

VIANVRAMState *via_nvram_init(ISABus *bus, int base_year, qemu_irq intercept_irq);
void via_nvram_set_cmos_data(VIANVRAMState *s, int addr, int val);
int via_nvram_get_cmos_data(VIANVRAMState *s, int addr);
void via_rtc_reset_reinjection(VIANVRAMState *rtc);

#endif /* HW_RTC_VIA_NVRAM_H */
