/*
 * VIA KT880 Desktop North Bridge
 *
 * Copyright (c) 2006 Fabrice Bellard
 * Copyright (c) 2025 Tisenu100
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef HW_PCI_KT880_H
#define HW_PCI_KT880_H

#include "hw/pci/pci_device.h"
#include "qom/object.h"

#define KT880_HOST_PROP_PCI_TYPE "pci-type"

#define TYPE_KT880_PCI_HOST_BRIDGE "KT880-pcihost"
#define TYPE_KT880_PCI_DEVICE "KT880"

OBJECT_DECLARE_SIMPLE_TYPE(PCIKT880State, KT880_PCI_DEVICE)

struct PCIKT880State {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    /* Shadow Interface */
    uint8_t block_status[10];
    MemoryRegion shadow_blocks[4][10];

    /* SMRAM*/
    MemoryRegion smram_region, smram, low_smram;
};

#define TYPE_IGD_PASSTHROUGH_KT880_PCI_DEVICE "igd-passthrough-KT880"

#endif
