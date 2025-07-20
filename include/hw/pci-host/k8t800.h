/*
 * VIA K8T800
 *
 * Copyright (c) 2006 Fabrice Bellard
 * Copyright (c) 2025 Tisenu100
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef HW_PCI_K8T800_H
#define HW_PCI_K8T800_H

#include "hw/pci/pci_device.h"
#include "hw/pci-host/pam.h"
#include "qom/object.h"

#define K8T800_HOST_PROP_PCI_TYPE "pci-type"

#define TYPE_K8T800_PCI_HOST_BRIDGE "k8t800-pcihost"
#define TYPE_K8T800_PCI_DEVICE "k8t800"

OBJECT_DECLARE_SIMPLE_TYPE(PCIK8T800State, K8T800_PCI_DEVICE)

struct PCIK8T800State {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    /* SRAM */
    uint8_t sram_index;
    uint8_t sram[256];
    MemoryRegion sram_io;

    /* Shadow RAM */
    int active_state[10];
    MemoryRegion shadow_region[10][4];

    /* SMRAM */
    MemoryRegion smram_region;
    MemoryRegion smram, smbase, low_smram;
};

#define TYPE_IGD_PASSTHROUGH_K8T800_PCI_DEVICE "igd-passthrough-k8t800"

#endif
