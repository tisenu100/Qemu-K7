/*
 * VIA NVRAM
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
 * Copyright (c) 2025 Tisenu100
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/module.h"
#include "qemu/bcd.h"
#include "qemu/qemu-print.h"
#include "hw/acpi/acpi_aml_interface.h"
#include "hw/intc/kvm_irqcount.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "qemu/timer.h"
#include "system/system.h"
#include "system/replay.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/rtc.h"
#include "hw/rtc/via_nvram.h"
#include "hw/rtc/mc146818rtc_regs.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/qapi-events-misc.h"
#include "qapi/visitor.h"

static void rtc_set_time(VIANVRAMState *s);
static void rtc_update_time(VIANVRAMState *s);
static void rtc_set_cmos(VIANVRAMState *s, const struct tm *tm);
static inline int rtc_from_bcd(VIANVRAMState *s, int a);
static uint64_t get_next_alarm(VIANVRAMState *s);

static inline bool rtc_running(VIANVRAMState *s)
{
    return (!(s->cmos_data[RTC_REG_B] & REG_B_SET) &&
            (s->cmos_data[RTC_REG_A] & 0x70) <= 0x20);
}

static uint64_t get_guest_rtc_ns(VIANVRAMState *s)
{
    uint64_t guest_clock = qemu_clock_get_ns(rtc_clock);

    return s->base_rtc * NANOSECONDS_PER_SECOND +
        guest_clock - s->last_update + s->offset;
}

static void rtc_coalesced_timer_update(VIANVRAMState *s)
{
    if (s->irq_coalesced == 0) {
        timer_del(s->coalesced_timer);
    } else {
        /* divide each RTC interval to 2 - 8 smaller intervals */
        int c = MIN(s->irq_coalesced, 7) + 1;
        int64_t next_clock = qemu_clock_get_ns(rtc_clock) +
            periodic_clock_to_ns(s->period / c);
        timer_mod(s->coalesced_timer, next_clock);
    }
}

void via_rtc_reset_reinjection(VIANVRAMState *rtc)
{
    rtc->irq_coalesced = 0;
}

static bool rtc_policy_slew_deliver_irq(VIANVRAMState *s)
{
    kvm_reset_irq_delivered();
    qemu_irq_raise(s->irq);
    return kvm_get_irq_delivered();
}

static void rtc_coalesced_timer(void *opaque)
{
    VIANVRAMState *s = opaque;

    if (s->irq_coalesced != 0) {
        s->cmos_data[RTC_REG_C] |= 0xc0;
        if (rtc_policy_slew_deliver_irq(s)) {
            s->irq_coalesced--;
        }
    }

    rtc_coalesced_timer_update(s);
}

static uint32_t rtc_periodic_clock_ticks(VIANVRAMState *s)
{
    int period_code;

    if (!(s->cmos_data[RTC_REG_B] & REG_B_PIE)) {
        return 0;
     }

    period_code = s->cmos_data[RTC_REG_A] & 0x0f;

    return periodic_period_to_clock(period_code);
}

/*
 * handle periodic timer. @old_period indicates the periodic timer update
 * is just due to period adjustment.
 */
static void periodic_timer_update(VIANVRAMState *s, int64_t current_time,
                                  uint32_t old_period, bool period_change)
{
    uint32_t period;
    int64_t cur_clock, next_irq_clock, lost_clock = 0;

    period = rtc_periodic_clock_ticks(s);
    s->period = period;

    if (!period) {
        s->irq_coalesced = 0;
        timer_del(s->periodic_timer);
        return;
    }

    /* compute 32 khz clock */
    cur_clock =
        muldiv64(current_time, 32768, NANOSECONDS_PER_SECOND);

    /*
     * if the periodic timer's update is due to period re-configuration,
     * we should count the clock since last interrupt.
     */
    if (old_period && period_change) {
        int64_t last_periodic_clock, next_periodic_clock;

        next_periodic_clock = muldiv64(s->next_periodic_time,
                                32768, NANOSECONDS_PER_SECOND);
        last_periodic_clock = next_periodic_clock - old_period;
        lost_clock = cur_clock - last_periodic_clock;
        assert(lost_clock >= 0);
    }

    /*
     * s->irq_coalesced can change for two reasons:
     *
     * a) if one or more periodic timer interrupts have been lost,
     *    lost_clock will be more that a period.
     *
     * b) when the period may be reconfigured, we expect the OS to
     *    treat delayed tick as the new period.  So, when switching
     *    from a shorter to a longer period, scale down the missing,
     *    because the OS will treat past delayed ticks as longer
     *    (leftovers are put back into lost_clock).  When switching
     *    to a shorter period, scale up the missing ticks since the
     *    OS handler will treat past delayed ticks as shorter.
     */
    if (s->lost_tick_policy == LOST_TICK_POLICY_SLEW) {
        uint32_t old_irq_coalesced = s->irq_coalesced;

        lost_clock += old_irq_coalesced * old_period;
        s->irq_coalesced = lost_clock / s->period;
        lost_clock %= s->period;
        if (old_irq_coalesced != s->irq_coalesced ||
            old_period != s->period) {
            rtc_coalesced_timer_update(s);
        }
    } else {
        /*
         * no way to compensate the interrupt if LOST_TICK_POLICY_SLEW
         * is not used, we should make the time progress anyway.
         */
        lost_clock = MIN(lost_clock, period);
    }

    assert(lost_clock >= 0 && lost_clock <= period);

    next_irq_clock = cur_clock + period - lost_clock;
    s->next_periodic_time = periodic_clock_to_ns(next_irq_clock) + 1;
    timer_mod(s->periodic_timer, s->next_periodic_time);
}

static void rtc_periodic_timer(void *opaque)
{
    VIANVRAMState *s = opaque;

    periodic_timer_update(s, s->next_periodic_time, s->period, false);
    s->cmos_data[RTC_REG_C] |= REG_C_PF;
    if (s->cmos_data[RTC_REG_B] & REG_B_PIE) {
        s->cmos_data[RTC_REG_C] |= REG_C_IRQF;
        if (s->lost_tick_policy == LOST_TICK_POLICY_SLEW) {
            if (s->irq_reinject_on_ack_count >= 20)
                s->irq_reinject_on_ack_count = 0;
            if (!rtc_policy_slew_deliver_irq(s)) {
                s->irq_coalesced++;
                rtc_coalesced_timer_update(s);
            }
        } else
            qemu_irq_raise(s->irq);
    }
}

/* handle update-ended timer */
static void check_update_timer(VIANVRAMState *s)
{
    uint64_t next_update_time;
    uint64_t guest_nsec;
    int next_alarm_sec;

    /* From the data sheet: "Holding the dividers in reset prevents
     * interrupts from operating, while setting the SET bit allows"
     * them to occur.
     */
    if ((s->cmos_data[RTC_REG_A] & 0x60) == 0x60) {
        assert((s->cmos_data[RTC_REG_A] & REG_A_UIP) == 0);
        timer_del(s->update_timer);
        return;
    }

    guest_nsec = get_guest_rtc_ns(s) % NANOSECONDS_PER_SECOND;
    next_update_time = qemu_clock_get_ns(rtc_clock)
        + NANOSECONDS_PER_SECOND - guest_nsec;

    /* Compute time of next alarm.  One second is already accounted
     * for in next_update_time.
     */
    next_alarm_sec = get_next_alarm(s);
    s->next_alarm_time = next_update_time +
                         (next_alarm_sec - 1) * NANOSECONDS_PER_SECOND;

    /* If update_in_progress latched the UIP bit, we must keep the timer
     * programmed to the next second, so that UIP is cleared.  Otherwise,
     * if UF is already set, we might be able to optimize.
     */
    if (!(s->cmos_data[RTC_REG_A] & REG_A_UIP) &&
        (s->cmos_data[RTC_REG_C] & REG_C_UF)) {
        /* If AF cannot change (i.e. either it is set already, or
         * SET=1 and then the time is not updated), nothing to do.
         */
        if ((s->cmos_data[RTC_REG_B] & REG_B_SET) ||
            (s->cmos_data[RTC_REG_C] & REG_C_AF)) {
            timer_del(s->update_timer);
            return;
        }

        /* UF is set, but AF is clear.  Program the timer to target
         * the alarm time.  */
        next_update_time = s->next_alarm_time;
    }
    if (next_update_time != timer_expire_time_ns(s->update_timer)) {
        timer_mod(s->update_timer, next_update_time);
    }
}

static inline uint8_t convert_hour(VIANVRAMState *s, uint8_t hour)
{
    if (!(s->cmos_data[RTC_REG_B] & REG_B_24H)) {
        hour %= 12;
        if (s->cmos_data[RTC_HOURS] & 0x80) {
            hour += 12;
        }
    }
    return hour;
}

static uint64_t get_next_alarm(VIANVRAMState *s)
{
    int32_t alarm_sec, alarm_min, alarm_hour, cur_hour, cur_min, cur_sec;
    int32_t hour, min, sec;

    rtc_update_time(s);

    alarm_sec = rtc_from_bcd(s, s->cmos_data[RTC_SECONDS_ALARM]);
    alarm_min = rtc_from_bcd(s, s->cmos_data[RTC_MINUTES_ALARM]);
    alarm_hour = rtc_from_bcd(s, s->cmos_data[RTC_HOURS_ALARM]);
    alarm_hour = alarm_hour == -1 ? -1 : convert_hour(s, alarm_hour);

    cur_sec = rtc_from_bcd(s, s->cmos_data[RTC_SECONDS]);
    cur_min = rtc_from_bcd(s, s->cmos_data[RTC_MINUTES]);
    cur_hour = rtc_from_bcd(s, s->cmos_data[RTC_HOURS]);
    cur_hour = convert_hour(s, cur_hour);

    if (alarm_hour == -1) {
        alarm_hour = cur_hour;
        if (alarm_min == -1) {
            alarm_min = cur_min;
            if (alarm_sec == -1) {
                alarm_sec = cur_sec + 1;
            } else if (cur_sec > alarm_sec) {
                alarm_min++;
            }
        } else if (cur_min == alarm_min) {
            if (alarm_sec == -1) {
                alarm_sec = cur_sec + 1;
            } else {
                if (cur_sec > alarm_sec) {
                    alarm_hour++;
                }
            }
            if (alarm_sec == 60) {
                /* wrap to next hour, minutes is not in don't care mode */
                alarm_sec = 0;
                alarm_hour++;
            }
        } else if (cur_min > alarm_min) {
            alarm_hour++;
        }
    } else if (cur_hour == alarm_hour) {
        if (alarm_min == -1) {
            alarm_min = cur_min;
            if (alarm_sec == -1) {
                alarm_sec = cur_sec + 1;
            } else if (cur_sec > alarm_sec) {
                alarm_min++;
            }

            if (alarm_sec == 60) {
                alarm_sec = 0;
                alarm_min++;
            }
            /* wrap to next day, hour is not in don't care mode */
            alarm_min %= 60;
        } else if (cur_min == alarm_min) {
            if (alarm_sec == -1) {
                alarm_sec = cur_sec + 1;
            }
            /* wrap to next day, hours+minutes not in don't care mode */
            alarm_sec %= 60;
        }
    }

    /* values that are still don't care fire at the next min/sec */
    if (alarm_min == -1) {
        alarm_min = 0;
    }
    if (alarm_sec == -1) {
        alarm_sec = 0;
    }

    /* keep values in range */
    if (alarm_sec == 60) {
        alarm_sec = 0;
        alarm_min++;
    }
    if (alarm_min == 60) {
        alarm_min = 0;
        alarm_hour++;
    }
    alarm_hour %= 24;

    hour = alarm_hour - cur_hour;
    min = hour * 60 + alarm_min - cur_min;
    sec = min * 60 + alarm_sec - cur_sec;
    return sec <= 0 ? sec + 86400 : sec;
}

static void rtc_update_timer(void *opaque)
{
    VIANVRAMState *s = opaque;
    int32_t irqs = REG_C_UF;
    int32_t new_irqs;

    assert((s->cmos_data[RTC_REG_A] & 0x60) != 0x60);

    /* UIP might have been latched, update time and clear it.  */
    rtc_update_time(s);
    s->cmos_data[RTC_REG_A] &= ~REG_A_UIP;

    if (qemu_clock_get_ns(rtc_clock) >= s->next_alarm_time) {
        irqs |= REG_C_AF;
        if (s->cmos_data[RTC_REG_B] & REG_B_AIE) {
            qemu_system_wakeup_request(QEMU_WAKEUP_REASON_RTC, NULL);
        }
    }

    new_irqs = irqs & ~s->cmos_data[RTC_REG_C];
    s->cmos_data[RTC_REG_C] |= irqs;
    if ((new_irqs & s->cmos_data[RTC_REG_B]) != 0) {
        s->cmos_data[RTC_REG_C] |= REG_C_IRQF;
        qemu_irq_raise(s->irq);
    }
    check_update_timer(s);
}

static void cmos_ioport_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    VIANVRAMState *s = opaque;
    uint32_t old_period;
    bool update_periodic_timer;

    if ((addr & 1) == 0) {
        s->cmos_index = data & 0x7f;
    } else {
        switch(s->cmos_index) {
        case RTC_SECONDS_ALARM:
        case RTC_MINUTES_ALARM:
        case RTC_HOURS_ALARM:
            s->cmos_data[s->cmos_index] = data;
            check_update_timer(s);
            break;
        case RTC_IBM_PS2_CENTURY_BYTE:
            s->cmos_index = RTC_CENTURY;
            /* fall through */
        case RTC_CENTURY:
        case RTC_SECONDS:
        case RTC_MINUTES:
        case RTC_HOURS:
        case RTC_DAY_OF_WEEK:
        case RTC_DAY_OF_MONTH:
        case RTC_MONTH:
        case RTC_YEAR:
            s->cmos_data[s->cmos_index] = data;
            /* if in set mode, do not update the time */
            if (rtc_running(s)) {
                rtc_set_time(s);
                check_update_timer(s);
            }
            break;
        case RTC_REG_A:
            update_periodic_timer = (s->cmos_data[RTC_REG_A] ^ data) & 0x0f;
            old_period = rtc_periodic_clock_ticks(s);

            if ((data & 0x60) == 0x60) {
                if (rtc_running(s)) {
                    rtc_update_time(s);
                }
                /* What happens to UIP when divider reset is enabled is
                 * unclear from the datasheet.  Shouldn't matter much
                 * though.
                 */
                s->cmos_data[RTC_REG_A] &= ~REG_A_UIP;
            } else if (((s->cmos_data[RTC_REG_A] & 0x60) == 0x60) &&
                    (data & 0x70)  <= 0x20) {
                /* when the divider reset is removed, the first update cycle
                 * begins one-half second later*/
                if (!(s->cmos_data[RTC_REG_B] & REG_B_SET)) {
                    s->offset = 500000000;
                    rtc_set_time(s);
                }
                s->cmos_data[RTC_REG_A] &= ~REG_A_UIP;
            }
            /* UIP bit is read only */
            s->cmos_data[RTC_REG_A] = (data & ~REG_A_UIP) |
                (s->cmos_data[RTC_REG_A] & REG_A_UIP);

            if (update_periodic_timer) {
                periodic_timer_update(s, qemu_clock_get_ns(rtc_clock),
                                      old_period, true);
            }

            check_update_timer(s);
            break;
        case RTC_REG_B:
            update_periodic_timer = (s->cmos_data[RTC_REG_B] ^ data)
                                       & REG_B_PIE;
            old_period = rtc_periodic_clock_ticks(s);

            if (data & REG_B_SET) {
                /* update cmos to when the rtc was stopping */
                if (rtc_running(s)) {
                    rtc_update_time(s);
                }
                /* set mode: reset UIP mode */
                s->cmos_data[RTC_REG_A] &= ~REG_A_UIP;
                data &= ~REG_B_UIE;
            } else {
                /* if disabling set mode, update the time */
                if ((s->cmos_data[RTC_REG_B] & REG_B_SET) &&
                    (s->cmos_data[RTC_REG_A] & 0x70) <= 0x20) {
                    s->offset = get_guest_rtc_ns(s) % NANOSECONDS_PER_SECOND;
                    rtc_set_time(s);
                }
            }
            /* if an interrupt flag is already set when the interrupt
             * becomes enabled, raise an interrupt immediately.  */
            if (data & s->cmos_data[RTC_REG_C] & REG_C_MASK) {
                s->cmos_data[RTC_REG_C] |= REG_C_IRQF;
                qemu_irq_raise(s->irq);
            } else {
                s->cmos_data[RTC_REG_C] &= ~REG_C_IRQF;
                qemu_irq_lower(s->irq);
            }
            s->cmos_data[RTC_REG_B] = data;

            if (update_periodic_timer) {
                periodic_timer_update(s, qemu_clock_get_ns(rtc_clock),
                                      old_period, true);
            }

            check_update_timer(s);
            break;
        case RTC_REG_C:
        case RTC_REG_D:
            /* cannot write to them */
            break;
        default:
            s->cmos_data[s->cmos_index] = data;
            break;
        }
    }
}


static void cmos_extended_ioport_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    VIANVRAMState *s = opaque;
    if(addr)
        if(s->cmos_index > 0x7f)
            s->cmos_data[s->cmos_index] = data;
        else 
            cmos_extended_ioport_write(s, addr, data, size);
    else
        s->cmos_index = data;
}


static inline int rtc_to_bcd(VIANVRAMState *s, int a)
{
    if (s->cmos_data[RTC_REG_B] & REG_B_DM) {
        return a;
    } else {
        return ((a / 10) << 4) | (a % 10);
    }
}

static inline int rtc_from_bcd(VIANVRAMState *s, int a)
{
    if ((a & 0xc0) == 0xc0) {
        return -1;
    }
    if (s->cmos_data[RTC_REG_B] & REG_B_DM) {
        return a;
    } else {
        return ((a >> 4) * 10) + (a & 0x0f);
    }
}

static void rtc_get_time(VIANVRAMState *s, struct tm *tm)
{
    tm->tm_sec = rtc_from_bcd(s, s->cmos_data[RTC_SECONDS]);
    tm->tm_min = rtc_from_bcd(s, s->cmos_data[RTC_MINUTES]);
    tm->tm_hour = rtc_from_bcd(s, s->cmos_data[RTC_HOURS] & 0x7f);
    if (!(s->cmos_data[RTC_REG_B] & REG_B_24H)) {
        tm->tm_hour %= 12;
        if (s->cmos_data[RTC_HOURS] & 0x80) {
            tm->tm_hour += 12;
        }
    }
    tm->tm_wday = rtc_from_bcd(s, s->cmos_data[RTC_DAY_OF_WEEK]) - 1;
    tm->tm_mday = rtc_from_bcd(s, s->cmos_data[RTC_DAY_OF_MONTH]);
    tm->tm_mon = rtc_from_bcd(s, s->cmos_data[RTC_MONTH]) - 1;
    tm->tm_year =
        rtc_from_bcd(s, s->cmos_data[RTC_YEAR]) + s->base_year +
        rtc_from_bcd(s, s->cmos_data[RTC_CENTURY]) * 100 - 1900;
}

static void rtc_set_time(VIANVRAMState *s)
{
    struct tm tm = {};
    g_autofree const char *qom_path = object_get_canonical_path(OBJECT(s));

    rtc_get_time(s, &tm);
    s->base_rtc = mktimegm(&tm);
    s->last_update = qemu_clock_get_ns(rtc_clock);

    qapi_event_send_rtc_change(qemu_timedate_diff(&tm), qom_path);
}

static void rtc_set_cmos(VIANVRAMState *s, const struct tm *tm)
{
    int year;

    s->cmos_data[RTC_SECONDS] = rtc_to_bcd(s, tm->tm_sec);
    s->cmos_data[RTC_MINUTES] = rtc_to_bcd(s, tm->tm_min);
    if (s->cmos_data[RTC_REG_B] & REG_B_24H) {
        /* 24 hour format */
        s->cmos_data[RTC_HOURS] = rtc_to_bcd(s, tm->tm_hour);
    } else {
        /* 12 hour format */
        int h = (tm->tm_hour % 12) ? tm->tm_hour % 12 : 12;
        s->cmos_data[RTC_HOURS] = rtc_to_bcd(s, h);
        if (tm->tm_hour >= 12)
            s->cmos_data[RTC_HOURS] |= 0x80;
    }
    s->cmos_data[RTC_DAY_OF_WEEK] = rtc_to_bcd(s, tm->tm_wday + 1);
    s->cmos_data[RTC_DAY_OF_MONTH] = rtc_to_bcd(s, tm->tm_mday);
    s->cmos_data[RTC_MONTH] = rtc_to_bcd(s, tm->tm_mon + 1);
    year = tm->tm_year + 1900 - s->base_year;
    s->cmos_data[RTC_YEAR] = rtc_to_bcd(s, year % 100);
    s->cmos_data[RTC_CENTURY] = rtc_to_bcd(s, year / 100);
}

static void rtc_update_time(VIANVRAMState *s)
{
    struct tm ret;
    time_t guest_sec;
    int64_t guest_nsec;

    guest_nsec = get_guest_rtc_ns(s);
    guest_sec = guest_nsec / NANOSECONDS_PER_SECOND;
    gmtime_r(&guest_sec, &ret);

    /* Is SET flag of Register B disabled? */
    if ((s->cmos_data[RTC_REG_B] & REG_B_SET) == 0) {
        rtc_set_cmos(s, &ret);
    }
}

static int update_in_progress(VIANVRAMState *s)
{
    int64_t guest_nsec;

    if (!rtc_running(s)) {
        return 0;
    }
    if (timer_pending(s->update_timer)) {
        int64_t next_update_time = timer_expire_time_ns(s->update_timer);
        /* Latch UIP until the timer expires.  */
        if (qemu_clock_get_ns(rtc_clock) >=
            (next_update_time - (8 * NANOSECONDS_PER_SECOND / 32768))) {
            s->cmos_data[RTC_REG_A] |= REG_A_UIP;
            return 1;
        }
    }

    guest_nsec = get_guest_rtc_ns(s);
    /* UIP bit will be set at last 244us of every second. */
    if ((guest_nsec % NANOSECONDS_PER_SECOND) >=
        (NANOSECONDS_PER_SECOND - (8 * NANOSECONDS_PER_SECOND / 32768))) {
        return 1;
    }
    return 0;
}

static uint64_t cmos_ioport_read(void *opaque, hwaddr addr, unsigned size)
{
    VIANVRAMState *s = opaque;
    int ret;
    if ((addr & 1) == 0) {
        return 0xff;
    } else {
        switch(s->cmos_index) {
        case RTC_IBM_PS2_CENTURY_BYTE:
            s->cmos_index = RTC_CENTURY;
            /* fall through */
        case RTC_CENTURY:
        case RTC_SECONDS:
        case RTC_MINUTES:
        case RTC_HOURS:
        case RTC_DAY_OF_WEEK:
        case RTC_DAY_OF_MONTH:
        case RTC_MONTH:
        case RTC_YEAR:
            /* if not in set mode, calibrate cmos before
             * reading*/
            if (rtc_running(s)) {
                rtc_update_time(s);
            }
            ret = s->cmos_data[s->cmos_index];
            break;
        case RTC_REG_A:
            ret = s->cmos_data[s->cmos_index];
            if (update_in_progress(s)) {
                ret |= REG_A_UIP;
            }
            break;
        case RTC_REG_C:
            ret = s->cmos_data[s->cmos_index];
            qemu_irq_lower(s->irq);
            s->cmos_data[RTC_REG_C] = 0x00;
            if (ret & (REG_C_UF | REG_C_AF)) {
                check_update_timer(s);
            }

            if(s->irq_coalesced &&
                    (s->cmos_data[RTC_REG_B] & REG_B_PIE) &&
                    s->irq_reinject_on_ack_count < 20) {
                s->irq_reinject_on_ack_count++;
                s->cmos_data[RTC_REG_C] |= REG_C_IRQF | REG_C_PF;
                if (rtc_policy_slew_deliver_irq(s)) {
                    s->irq_coalesced--;
                }
            }
            break;
        default:
            ret = s->cmos_data[s->cmos_index];
            break;
        }
        return ret;
    }
}


static uint64_t cmos_extended_ioport_read(void *opaque, hwaddr addr, unsigned size)
{
    VIANVRAMState *s = opaque;
    int ret;

    if(addr)
        if(s->cmos_index > 0x7f)
            ret = s->cmos_data[s->cmos_index];
        else
            ret = cmos_ioport_read(s, addr, size);
    else
        ret = s->cmos_index;

    return ret;
}


void via_nvram_set_cmos_data(VIANVRAMState *s, int addr, int val)
{
    if (addr >= 0 && addr <= 127)
        s->cmos_data[addr] = val;
}

int via_nvram_get_cmos_data(VIANVRAMState *s, int addr)
{
    assert(addr >= 0 && addr <= 127);
    return s->cmos_data[addr];
}

static void rtc_set_date_from_host(ISADevice *dev)
{
    VIANVRAMState *s = VIA_NVRAM(dev);
    struct tm tm;

    qemu_get_timedate(&tm, 0);

    s->base_rtc = mktimegm(&tm);
    s->last_update = qemu_clock_get_ns(rtc_clock);
    s->offset = 0;

    /* set the CMOS date */
    rtc_set_cmos(s, &tm);
}

static int rtc_pre_save(void *opaque)
{
    VIANVRAMState *s = opaque;

    rtc_update_time(s);

    return 0;
}

static int rtc_post_load(void *opaque, int version_id)
{
    VIANVRAMState *s = opaque;

    if (version_id <= 2 || rtc_clock == QEMU_CLOCK_REALTIME) {
        rtc_set_time(s);
        s->offset = 0;
        check_update_timer(s);
    }
    s->period = rtc_periodic_clock_ticks(s);

    /* The periodic timer is deterministic in record/replay mode,
     * so there is no need to update it after loading the vmstate.
     * Reading RTC here would misalign record and replay.
     */
    if (replay_mode == REPLAY_MODE_NONE) {
        uint64_t now = qemu_clock_get_ns(rtc_clock);
        if (now < s->next_periodic_time ||
            now > (s->next_periodic_time + get_max_clock_jump())) {
            periodic_timer_update(s, qemu_clock_get_ns(rtc_clock), s->period, false);
        }
    }

    if (version_id >= 2) {
        if (s->lost_tick_policy == LOST_TICK_POLICY_SLEW) {
            rtc_coalesced_timer_update(s);
        }
    }
    return 0;
}

static bool rtc_irq_reinject_on_ack_count_needed(void *opaque)
{
    VIANVRAMState *s = (VIANVRAMState *)opaque;
    return s->irq_reinject_on_ack_count != 0;
}

static const VMStateDescription vmstate_rtc_irq_reinject_on_ack_count = {
    .name = "via_nvram/irq_reinject_on_ack_count",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = rtc_irq_reinject_on_ack_count_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(irq_reinject_on_ack_count, VIANVRAMState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_rtc = {
    .name = "VIA NVRAM",
    .version_id = 3,
    .minimum_version_id = 3,
    .pre_save = rtc_pre_save,
    .post_load = rtc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BUFFER(cmos_data, VIANVRAMState),
        VMSTATE_UINT8(cmos_index, VIANVRAMState),
        VMSTATE_UNUSED(7*4),
        VMSTATE_TIMER_PTR(periodic_timer, VIANVRAMState),
        VMSTATE_INT64(next_periodic_time, VIANVRAMState),
        VMSTATE_UNUSED(3*8),
        VMSTATE_UINT32(irq_coalesced, VIANVRAMState),
        VMSTATE_UINT32(period, VIANVRAMState),
        VMSTATE_UINT64(base_rtc, VIANVRAMState),
        VMSTATE_UINT64(last_update, VIANVRAMState),
        VMSTATE_INT64(offset, VIANVRAMState),
        VMSTATE_TIMER_PTR(update_timer, VIANVRAMState),
        VMSTATE_UINT64(next_alarm_time, VIANVRAMState),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_rtc_irq_reinject_on_ack_count,
        NULL
    }
};

/* set CMOS shutdown status register (index 0xF) as S3_resume(0xFE)
   BIOS will read it and start S3 resume at POST Entry */
static void rtc_notify_suspend(Notifier *notifier, void *data)
{
    VIANVRAMState *s = container_of(notifier, VIANVRAMState,
                                       suspend_notifier);
    via_nvram_set_cmos_data(s, 0xF, 0xFE);
}

static const MemoryRegionOps cmos_ops = {
    .read = cmos_ioport_read,
    .write = cmos_ioport_write,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};


static const MemoryRegionOps cmos_extended_ops = {
    .read = cmos_extended_ioport_read,
    .write = cmos_extended_ioport_write,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void rtc_get_date(Object *obj, struct tm *current_tm, Error **errp)
{
    VIANVRAMState *s = VIA_NVRAM(obj);

    rtc_update_time(s);
    rtc_get_time(s, current_tm);
}

static void rtc_realizefn(DeviceState *dev, Error **errp)
{
    ISADevice *isadev = ISA_DEVICE(dev);
    VIANVRAMState *s = VIA_NVRAM(dev);

    s->cmos_data[RTC_REG_A] = 0x26;
    s->cmos_data[RTC_REG_B] = 0x02;
    s->cmos_data[RTC_REG_C] = 0x00;
    s->cmos_data[RTC_REG_D] = 0x80;

    for(int i = 0x80; i < 0xff; i++) {
        s->cmos_data[i] = 0xff;
    }

    /* This is for historical reasons.  The default base year qdev property
     * was set to 2000 for most machine types before the century byte was
     * implemented.
     *
     * This if statement means that the century byte will be always 0
     * (at least until 2079...) for base_year = 1980, but will be set
     * correctly for base_year = 2000.
     */
    if (s->base_year == 2000) {
        s->base_year = 0;
    }

    if (s->isairq >= ISA_NUM_IRQS) {
        error_setg(errp, "Maximum value for \"irq\" is: %u", ISA_NUM_IRQS - 1);
        return;
    }

    rtc_set_date_from_host(isadev);

    switch (s->lost_tick_policy) {
    case LOST_TICK_POLICY_SLEW:
        s->coalesced_timer =
            timer_new_ns(rtc_clock, rtc_coalesced_timer, s);
        break;
    case LOST_TICK_POLICY_DISCARD:
        break;
    default:
        error_setg(errp, "Invalid lost tick policy.");
        return;
    }

    s->periodic_timer = timer_new_ns(rtc_clock, rtc_periodic_timer, s);
    s->update_timer = timer_new_ns(rtc_clock, rtc_update_timer, s);
    check_update_timer(s);

    s->suspend_notifier.notify = rtc_notify_suspend;
    qemu_register_suspend_notifier(&s->suspend_notifier);

    memory_region_init_io(&s->io, OBJECT(s), &cmos_ops, s, "rtc", 2);
    isa_register_ioport(isadev, &s->io, s->io_base);

    /* register rtc 0x70 port for coalesced_pio */
    memory_region_set_flush_coalesced(&s->io);
    memory_region_init_io(&s->coalesced_io, OBJECT(s), &cmos_ops, s, "rtc-index", 1);
    memory_region_add_subregion(&s->io, 0, &s->coalesced_io);
    memory_region_add_coalescing(&s->coalesced_io, 0, 1);

    /* Extended RTC for 256-byte access */
    memory_region_init_io(&s->extended_io, OBJECT(s), &cmos_extended_ops, s, "rtc", 2);
    isa_register_ioport(isadev, &s->extended_io, s->extended_io_base);

    object_property_add_tm(OBJECT(s), "date", rtc_get_date);

    qdev_init_gpio_out(dev, &s->irq, 1);
}

VIANVRAMState *via_nvram_init(ISABus *bus, int base_year, qemu_irq intercept_irq)
{
    DeviceState *dev;
    ISADevice *isadev;
    VIANVRAMState *s;

    isadev = isa_new(TYPE_VIA_NVRAM);
    dev = DEVICE(isadev);
    s = VIA_NVRAM(isadev);
    qdev_prop_set_int32(dev, "base_year", base_year);
    isa_realize_and_unref(isadev, bus, &error_fatal);
    if (intercept_irq) {
        qdev_connect_gpio_out(dev, 0, intercept_irq);
    } else {
        isa_connect_gpio_out(isadev, 0, s->isairq);
    }

    object_property_add_alias(qdev_get_machine(), "rtc-time", OBJECT(isadev), "date");

    return s;
}

static const Property via_nvram_properties[] = {
    DEFINE_PROP_INT32("base_year", VIANVRAMState, base_year, 1980),
    DEFINE_PROP_UINT16("iobase", VIANVRAMState, io_base, 0x70),
    DEFINE_PROP_UINT16("extendediobase", VIANVRAMState, extended_io_base, 0x74),
    DEFINE_PROP_UINT8("irq", VIANVRAMState, isairq, RTC_ISA_IRQ),
    DEFINE_PROP_LOSTTICKPOLICY("lost_tick_policy", VIANVRAMState, lost_tick_policy, LOST_TICK_POLICY_DISCARD),
};

static void rtc_reset_enter(Object *obj, ResetType type)
{
    VIANVRAMState *s = VIA_NVRAM(obj);

    s->cmos_data[RTC_REG_B] &= ~(REG_B_PIE | REG_B_AIE | REG_B_SQWE);
    s->cmos_data[RTC_REG_C] &= ~(REG_C_UF | REG_C_IRQF | REG_C_PF | REG_C_AF);
    check_update_timer(s);


    if (s->lost_tick_policy == LOST_TICK_POLICY_SLEW) {
        s->irq_coalesced = 0;
        s->irq_reinject_on_ack_count = 0;
    }
}

static void rtc_reset_hold(Object *obj, ResetType type)
{
    VIANVRAMState *s = VIA_NVRAM(obj);

    qemu_irq_lower(s->irq);
}

static void rtc_class_initfn(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = rtc_realizefn;
    dc->vmsd = &vmstate_rtc;
    rc->phases.enter = rtc_reset_enter;
    rc->phases.hold = rtc_reset_hold;
    device_class_set_props(dc, via_nvram_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo via_nvram_info = {
    .name          = TYPE_VIA_NVRAM,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(VIANVRAMState),
    .class_init    = rtc_class_initfn,
    .interfaces = (const InterfaceInfo[]) {
        { },
    },
};

static void via_nvram_register_types(void)
{
    type_register_static(&via_nvram_info);
}

type_init(via_nvram_register_types)
