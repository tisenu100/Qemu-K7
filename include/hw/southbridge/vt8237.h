/*
 * VIA VT8237
 *
 * Copyright (c) 2006 Fabrice Bellard
 * Copyright (c) 2018 Hervé Poussineau
 * Copyright (c) 2025 Tisenu100
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef HW_SOUTHBRIDGE_VT8237_H
#define HW_SOUTHBRIDGE_VT8237_H

#include "hw/pci/pci_device.h"
#include "hw/acpi/acpi.h"
#include "hw/ide/pci.h"
#include "hw/rtc/via_nvram.h"
#include "hw/isa/apm.h"
#include "hw/i2c/pm_smbus.h"
#include "hw/usb/hcd-uhci.h"

struct VT8237State {
    PCIDevice dev;

    /* Bus Interrupts */
    qemu_irq cpu_intr;
    qemu_irq isa_irqs_in[24];

    /* Keyboard Wakeup */
    MemoryRegion kbd_wakeup_io;
    uint8_t kbd_wakeup_index;
    uint8_t kbd_wakeup_value[16];

    /* NVRAM */
    VIANVRAMState rtc;

    /* ACPI Interrupts */
    qemu_irq smi_irq;
    qemu_irq sci_irq;

    /* ACPI logic */
    APMState apm;
    MemoryRegion acpi_io;
    ACPIREGS ar;

    uint8_t via_acpi_regs[256];
    MemoryRegion via_acpi_io;

    /* SMBus logic */
    MemoryRegion smb_io;
    PMSMBus smb;
};

#define TYPE_VT8237_PCI_DEVICE "pci-vt8237"
OBJECT_DECLARE_SIMPLE_TYPE(VT8237State, VT8237_PCI_DEVICE)

#endif
