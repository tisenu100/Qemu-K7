/*
 * VIA VT8237
 *
 * Copyright (c) 2006 Fabrice Bellard
 * Copyright (c) 2018 Hervé Poussineau
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
#include "qemu/range.h"
#include "qemu/qemu-print.h"
#include "qapi/error.h"
#include "hw/dma/i8257.h"
#include "hw/southbridge/vt8237.h"
#include "hw/timer/i8254.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/i2c/pm_smbus.h"
#include "hw/isa/apm.h"
#include "hw/isa/isa.h"
#include "hw/pci/pci.h"
#include "system/runstate.h"
#include "migration/vmstate.h"
#include "qapi-events-run-state.h"

static void via_kbd_wakeup_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    VT8237State *s = opaque;

    if(addr) {
        qemu_printf("VIA VT8237 KB Wakeup: Writing 0x%02x to register 0xe%x\n", (uint8_t)val, s->kbd_wakeup_index);
        s->kbd_wakeup_value[s->kbd_wakeup_index] = val;
    }
    else {
        s->kbd_wakeup_index = val & 0x0f;
    }
}

static uint64_t via_kbd_wakeup_read(void *opaque, hwaddr addr, unsigned size)
{
    VT8237State *s = opaque;

    if(addr) {
        return s->kbd_wakeup_value[s->kbd_wakeup_index];
    }
    else {
        return s->kbd_wakeup_index;
    }
}

static const MemoryRegionOps via_kbd_wakeup_ops = {
    .read = via_kbd_wakeup_read,
    .write = via_kbd_wakeup_write,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void vt8237_pci_reset(PCIDevice *pci_dev)
{
    if(pci_get_byte(pci_dev->config + 0x4f) & 1) {
        qemu_printf("VIA VT8237: PCI Software Reset was called!\n");
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

static void vt8237_kbd_wakeup_update(VT8237State *s)
{
    PCIDevice *pci_dev = PCI_DEVICE(s);

    memory_region_transaction_begin();

    memory_region_set_enabled(&s->kbd_wakeup_io, false);

    if(pci_get_byte(pci_dev->config + 0x51) & 2) {
        memory_region_set_enabled(&s->kbd_wakeup_io, true);
        qemu_printf("VIA VT8237: Keyboard Wakeup I/O was enabled\n");
    }

    memory_region_transaction_commit();
}


static int vt8237_get_irq(PCIDevice *pci_dev, int pin)
{
    uint8_t irq = 0;
    bool irq_shared = !!(pci_get_byte(pci_dev->config + 0x46) & 0x10);
    bool apic = !!(pci_get_byte(pci_dev->config + 0x58) & 0x40);

    if(apic) { /* APIC Registers */
        if(pin > 3)
            irq = 20 - (irq_shared - 4) + pin;
        else
            irq = 16 + pin;

        return irq & 0x1f;
    }

    switch(pin) { /* Legacy PIC Registers */
        case 0:
            irq = pci_get_byte(pci_dev->config + 0x55) >> 4;
        break;

        case 1:
            irq = pci_get_byte(pci_dev->config + 0x56);
        break;

        case 2:
            irq = pci_get_byte(pci_dev->config + 0x56) >> 4;
        break;

        case 3:
            irq = pci_get_byte(pci_dev->config + 0x57) >> 4;
        break;

        case 4:
            if(irq_shared)
                irq = pci_get_byte(pci_dev->config + 0x44);
            else
                irq = pci_get_byte(pci_dev->config + 0x55) >> 4;
        break;

        case 5:
            if(irq_shared)
                irq = pci_get_byte(pci_dev->config + 0x44) >> 4;
            else
                irq = pci_get_byte(pci_dev->config + 0x56);
        break;

        case 6:
            if(irq_shared)
                irq = pci_get_byte(pci_dev->config + 0x45);
            else
                irq = pci_get_byte(pci_dev->config + 0x56) >> 4;
        break;

        case 7:
            if(irq_shared)
                irq = pci_get_byte(pci_dev->config + 0x45) >> 4;
            else
                irq = pci_get_byte(pci_dev->config + 0x57) >> 4;
        break;
    }

    return irq & 0x0f;
}

static void vt8237_trigger_irq(void *opaque, int pin, int level)
{
    VT8237State *s = opaque;
    PCIDevice *pci_dev = PCI_DEVICE(s);
    uint8_t irq = 0;

    irq = vt8237_get_irq(pci_dev, pin);

    /* Trigger IRQ's individually when requested */
    /* IRQ => PIN */
    /* Note: Level is provided somewhere on Qemu's garbled PCI logic. However requesting it again manually works fine. */
    qemu_printf("VIA VT8237: PIN %c triggered IRQ %d\n", 'A' + pin, irq);
    qemu_set_irq(s->isa_irqs_in[irq], pci_bus_get_irq_level(pci_get_bus(pci_dev), pin));
}

static PCIINTxRoute vt8237_route_intx_pin_to_irq(void *opaque, int pin)
{
    PCIDevice *pci_dev = opaque;
    int irq = vt8237_get_irq(pci_dev, pin);
    PCIINTxRoute route;

    if (irq < 24) {
        route.mode = PCI_INTX_ENABLED;
        route.irq = irq;
    } else {
        route.mode = PCI_INTX_DISABLED;
        route.irq = -1;
    }
    return route;
}

static void vt8237_sci_update(VT8237State *d, int irq)
{
    irq &= 0x0f;

    d->sci_irq = d->isa_irqs_in[irq];

    if(irq)
        qemu_printf("VIA VT8237: SCI IRQ was updated to %d\n", irq);
}

static void pm_tmr_timer(ACPIREGS *ar)
{
    VT8237State *s = container_of(ar, VT8237State, ar);
    acpi_update_sci(&s->ar, s->sci_irq);
}

static uint64_t via_acpi_pm_cnt_read(void *opaque, hwaddr addr, unsigned width)
{
    ACPIREGS *ar = opaque;
    return ar->pm1.cnt.cnt >> addr * 8;
}

static void via_acpi_pm_cnt_write(void *opaque, hwaddr addr, uint64_t val, unsigned width)
{
    ACPIREGS *ar = opaque;
    VT8237State *s = container_of(ar, VT8237State, ar);

    if(val & 2) {
        if(!!(s->via_acpi_regs[0x0a] & 0x20)) {
            qemu_printf("VIA VT8237: A Global Release SMI occured!\n");
            qemu_irq_raise(s->smi_irq);
        }
    }

    if (addr == 1) {
        val = val << 8 | (ar->pm1.cnt.cnt & 0xff);
    }
    ar->pm1.cnt.cnt = val & ~(ACPI_BITMASK_SLEEP_ENABLE);

    if (val & ACPI_BITMASK_SLEEP_ENABLE) {
        uint16_t sus_typ = (val >> 10) & 7;
        switch (sus_typ) {
        case 1:
            qemu_system_suspend_request();
            break;
        case 2:
            qapi_event_send_suspend_disk();
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
            break;
        }
    }
}

/* Taken from hw/acpi/core.c */
static const MemoryRegionOps via_acpi_pm_cnt_ops = {
    .read = via_acpi_pm_cnt_read,
    .write = via_acpi_pm_cnt_write,
    .impl.min_access_size = 2,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t via_acpi_read(void *opaque, hwaddr addr, unsigned size)
{
    VT8237State *s = opaque;
    uint8_t cur_addr = addr + 0x20;

    qemu_printf("VIA VT8237 ACPI: Reading 0x%0x to Register 0x%02x\n", (uint8_t)s->via_acpi_regs[addr], cur_addr);

    return s->via_acpi_regs[addr];
}

static void via_acpi_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    VT8237State *s = opaque;
    uint8_t cur_addr = addr + 0x20;

    switch(cur_addr) {
        case 0x28: case 0x29: case 0x30: case 0x31:
        case 0x32: case 0x33: case 0x40: case 0x45:
        return;
    }

    s->via_acpi_regs[addr] = val;
    qemu_printf("VIA VT8237 ACPI: Writing 0x%02x to Register 0x%02x\n", (uint8_t)val, cur_addr);

    switch(cur_addr) {
        case 0x2c:
            if(s->via_acpi_regs[0x0c] & 1) {
                qemu_printf("VIA VT8237: A BIOS Release event occured!\n");
                s->ar.pm1.evt.sts |= 0x0020;
                qemu_irq_raise(s->sci_irq);
            }
        break;
        case 0x2f: /* SW SMI */
            if(!!(s->via_acpi_regs[0x0a] & 0x40)) {
                qemu_printf("VIA VT8237: An SMI was provoked!\n");
                qemu_irq_raise(s->smi_irq);
            }
        break;
    }
}

static const MemoryRegionOps via_acpi_ops = {
    .read = via_acpi_read,
    .write = via_acpi_write,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void vt8237_acpi_mapping_update(VT8237State *d)
{
    PCIDevice *pci_dev = PCI_DEVICE(d);

    bool enabled = !!(pci_get_byte(pci_dev->config + 0x81) & 0x80);
    uint16_t acpi_address = pci_get_word(pci_dev->config + 0x88) & 0xfff0;

    memory_region_transaction_begin();

    memory_region_set_enabled(&d->acpi_io, false);

    if(enabled && (acpi_address != 0)) {
        memory_region_set_address(&d->acpi_io, acpi_address);
        memory_region_set_enabled(&d->acpi_io, true);
        qemu_printf("VIA VT8237: ACPI was enabled at 0x%04x\n", acpi_address);
    } else qemu_printf("VIA VT8237: ACPI was disabled\n");

    memory_region_transaction_commit();
}

static void vt8237_smb_mapping_update(VT8237State *d)
{
    PCIDevice *pci_dev = PCI_DEVICE(d);

    bool enabled = pci_get_byte(pci_dev->config + 0xd2) & 1;
    uint16_t smb_address = pci_get_word(pci_dev->config + 0xd0) & 0xfff0;

    memory_region_transaction_begin();

    memory_region_set_enabled(&d->smb.io, false);

    if(enabled && (smb_address != 0)) {
        memory_region_set_address(&d->smb.io, smb_address);
        memory_region_set_enabled(&d->smb.io, true);
        qemu_printf("VIA VT8237: SMBus was enabled at 0x%04x\n", smb_address);
    } else qemu_printf("VIA VT8237: SMBus was disabled\n");

    memory_region_transaction_commit();
}

static void vt8237_write_config(PCIDevice *dev, uint32_t address, uint32_t val, int len)
{
    VT8237State *s = VT8237_PCI_DEVICE(dev);

    switch(address) {
        case 0x2c: case 0x2d: case 0x2e: case 0x2f:
        case 0xa1: case 0xa2: case 0xa3: case 0xc0:
        case 0xc1: case 0xc2: case 0xc3: case 0xd6:
        return;
    }

    pci_default_write_config(dev, address, val, len);
    qemu_printf("VIA VT8237: dev->pci_conf[0x%02x] = %x\n", address, val);

    switch(address) {
        case 0x44: case 0x45: case 0x46: case 0x55:
        case 0x56: case 0x57: case 0x58:
            /* Request an IRQ update for all pins */
            pci_bus_fire_intx_routing_notifier(pci_get_bus(&s->dev));
        break;

        case 0x4f:
            vt8237_pci_reset(dev);
        break;

        case 0x51:
            vt8237_kbd_wakeup_update(s);
        break;

        case 0x81: case 0x88: case 0x89:
            vt8237_acpi_mapping_update(s);
        break;

        case 0x82:
            val &= 0x0f;
            val |= 0x50;
            pci_default_write_config(dev, address, val, len);
            vt8237_sci_update(s, val & 0x0f);
        break;

        case 0xd0: case 0xd1: case 0xd2:
            vt8237_smb_mapping_update(s);
        break;
    }

    pci_set_byte(dev->config + 0x82, 0x40);
}

static void vt8237_reset(DeviceState *dev)
{
    VT8237State *d = VT8237_PCI_DEVICE(dev);
    PCIDevice *pci_dev = PCI_DEVICE(d);

    pci_set_word(pci_dev->config + PCI_COMMAND, PCI_COMMAND_MASTER | PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
    pci_set_word(pci_dev->config + PCI_STATUS, PCI_STATUS_DEVSEL_MEDIUM);
    pci_set_byte(pci_dev->config + PCI_LATENCY_TIMER, 0x16);
    pci_set_long(pci_dev->config + 0x2c, 0xd1041106);
    pci_set_byte(pci_dev->config + 0x34, 0x80);
    pci_set_byte(pci_dev->config + 0x4f, 0x00);
    pci_set_byte(pci_dev->config + 0x51, 0x0c);
    pci_set_byte(pci_dev->config + 0x58, 0x40);
    pci_set_byte(pci_dev->config + 0x67, 0x04);
    pci_set_byte(pci_dev->config + 0x81, 0x04);
    pci_set_byte(pci_dev->config + 0x82, 0x50); /* Unintentional but needed so the BIOS can start */
    pci_set_long(pci_dev->config + 0x88, 0x00000001);
    pci_set_byte(pci_dev->config + 0x95, 0x40);
    pci_set_long(pci_dev->config + 0xc0, 0x00020001);
    pci_set_long(pci_dev->config + 0xd0, 0x0001);

    vt8237_kbd_wakeup_update(d);

    pci_set_byte(pci_dev->config + 0x44, 0x00);
    pci_set_byte(pci_dev->config + 0x45, 0x00);
    pci_set_byte(pci_dev->config + 0x46, 0x00);
    pci_set_byte(pci_dev->config + 0x55, 0x00);
    pci_set_byte(pci_dev->config + 0x56, 0x00);
    pci_set_byte(pci_dev->config + 0x57, 0x00);
    pci_set_byte(pci_dev->config + 0x58, 0x00);
    pci_bus_fire_intx_routing_notifier(pci_get_bus(pci_dev));

    vt8237_acpi_mapping_update(d);

    pci_set_byte(pci_dev->config + 0x82, 0x00);
    vt8237_sci_update(d, 0);

    acpi_pm1_evt_reset(&d->ar);
    acpi_pm1_cnt_reset(&d->ar);
    acpi_gpe_reset(&d->ar);

    pci_set_byte(pci_dev->config + 0xd2, 0x00);
    vt8237_smb_mapping_update(d);
}

static const VMStateDescription vmstate_vt8237 = {
    .name = "VIA VT8237",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, VT8237State),
        VMSTATE_END_OF_LIST()
    },
};

static void pci_vt8237_realize(PCIDevice *dev, Error **errp)
{
    VT8237State *d = VT8237_PCI_DEVICE(dev);
    PCIBus *pci_bus = pci_get_bus(dev);
    ISABus *isa_bus;

    qemu_printf("VIA VT8237: Setting up the Bus\n");
    isa_bus = isa_bus_new(DEVICE(d), pci_address_space(dev), pci_address_space_io(dev), errp);
    if (!isa_bus) return;

    /* Keyboard Wakeup */
    d->kbd_wakeup_value[0x00] = 0x08;
    d->kbd_wakeup_value[0x01] = 0xe0;
    d->kbd_wakeup_value[0x09] = 0x09;

   memory_region_init_io(&d->kbd_wakeup_io, OBJECT(d), &via_kbd_wakeup_ops, d, "vt8237-kbd-wakeup", 2);
   memory_region_set_enabled(&d->kbd_wakeup_io, false);
   memory_region_add_subregion_overlap(pci_address_space_io(dev), 0x2e, &d->kbd_wakeup_io, 2);

    qdev_init_gpio_out_named(DEVICE(dev), &d->cpu_intr, "intr", 1);

    isa_bus_register_input_irqs(isa_bus, d->isa_irqs_in);

    /* PIT */
    qemu_printf("VIA VT8237: Setting up the PIT\n");
    i8254_pit_init(isa_bus, 0x40, 0, NULL);

    i8257_dma_init(OBJECT(dev), isa_bus, 0);

    /* RTC */
    qemu_printf("VIA VT8237: Waking up NVRAM\n");
    qdev_prop_set_int32(DEVICE(&d->rtc), "base_year", 2000);
    if (!qdev_realize(DEVICE(&d->rtc), BUS(isa_bus), errp)) return;

    uint32_t irq = object_property_get_uint(OBJECT(&d->rtc), "irq", &error_fatal);
    isa_connect_gpio_out(ISA_DEVICE(&d->rtc), 0, irq);

    qemu_printf("VIA VT8237: Registering Interrupts\n");
    pci_bus_irqs(pci_bus, vt8237_trigger_irq, d, 8);
    pci_bus_set_route_irq_fn(pci_bus, vt8237_route_intx_pin_to_irq);

    qemu_printf("VIA VT8237: Setting up ACPI\n");
    apm_init(dev, &d->apm, NULL, d); /* APM is defined but has no logic on VIA */
    memory_region_init(&d->acpi_io, OBJECT(d), "vt8237-acpi", 256); /* 256 bytes */
    memory_region_set_enabled(&d->acpi_io, false);
    memory_region_add_subregion(pci_address_space_io(dev), 0, &d->acpi_io);

    acpi_pm1_evt_init(&d->ar, pm_tmr_timer, &d->acpi_io);

    acpi_pm1_cnt_headless_init(&d->ar, &d->acpi_io, false, false, 2, false);
    memory_region_init_io(&d->ar.pm1.cnt.io, memory_region_owner(&d->acpi_io), &via_acpi_pm_cnt_ops, &d->ar, "acpi-cnt", 2);
    memory_region_add_subregion(&d->acpi_io, 4, &d->ar.pm1.cnt.io);

    acpi_pm_tmr_init(&d->ar, pm_tmr_timer, &d->acpi_io);
    acpi_gpe_init(&d->ar, 1);

    memory_region_init_io(&d->via_acpi_io, memory_region_owner(&d->acpi_io), &via_acpi_ops, d, "sw-smi", 224);
    memory_region_add_subregion(&d->acpi_io, 0x20, &d->via_acpi_io);

    /* SMBus */
    qemu_printf("VIA VT8237: Setting up SMBus\n");
    pm_smbus_init(DEVICE(dev), &d->smb, 0);
    memory_region_add_subregion(pci_address_space_io(dev), 0, &d->smb.io);
    memory_region_set_enabled(&d->smb.io, false);
}

static void pci_vt8237_init(Object *obj)
{
    VT8237State *d = VT8237_PCI_DEVICE(obj);

    /* Expose the SMI pin to the standard SMI trigger procedure */
    qdev_init_gpio_out_named(DEVICE(obj), &d->smi_irq, "smi-irq", 1);

    /* Expose the IRQ's so they can be linked to GSI's on pc_init */
    qdev_init_gpio_out_named(DEVICE(obj), d->isa_irqs_in, "isa-irqs", 24);

    object_initialize_child(obj, "rtc", &d->rtc, TYPE_VIA_NVRAM);
}

static void pci_vt8237_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = pci_vt8237_realize;
    k->config_write = vt8237_write_config;
    device_class_set_legacy_reset(dc, vt8237_reset);
    dc->desc        = "VIA VT8237 ISA Bridge";
    dc->hotpluggable   = false;
    k->vendor_id    = PCI_VENDOR_ID_VIA;
    k->device_id    = PCI_DEVICE_ID_VIA_8237_ISA;
    k->class_id     = PCI_CLASS_BRIDGE_ISA;
    dc->user_creatable = false;
    dc->vmsd = &vmstate_vt8237;
}

static const TypeInfo vt8237_pci_type_info = {
    .name = TYPE_VT8237_PCI_DEVICE,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(VT8237State),
    .instance_init = pci_vt8237_init,
    .class_init = pci_vt8237_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { TYPE_ACPI_DEVICE_IF },
        { },
    },
};

static void vt8237_register_types(void)
{
    type_register_static(&vt8237_pci_type_info);
}

type_init(vt8237_register_types)
