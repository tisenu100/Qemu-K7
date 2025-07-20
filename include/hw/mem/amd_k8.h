/*
 * AMD K8 Memory Configuration Controllers
 *
 * Copyright (c) 2025 Tisenu100
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef HW_AMD_K8_H
#define HW_AMD_K8_H

#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "qom/object.h"

#define TYPE_AMD_HT_PCI_DEVICE "amd-ht"
OBJECT_DECLARE_SIMPLE_TYPE(AMDHTState, AMD_HT_PCI_DEVICE)

struct AMDHTState {
    PCIDevice parent_obj;
};

#define TYPE_AMD_AM_PCI_DEVICE "amd-am"
OBJECT_DECLARE_SIMPLE_TYPE(AMDAMState, AMD_AM_PCI_DEVICE)

struct AMDAMState {
    PCIDevice parent_obj;

    uint8_t smram_region_reg;
    MemoryRegion smram_region[2];
};

extern void amd_am_set_smram_region(AMDAMState *dev, uint8_t reg);

#define TYPE_AMD_DRAM_PCI_DEVICE "amd-dram"
OBJECT_DECLARE_SIMPLE_TYPE(AMDDRAMState, AMD_DRAM_PCI_DEVICE)

struct AMDDRAMState {
    PCIDevice parent_obj;
};

#define TYPE_AMD_MC_PCI_DEVICE "amd-mc"
OBJECT_DECLARE_SIMPLE_TYPE(AMDMCState, AMD_MC_PCI_DEVICE)

struct AMDMCState {
    PCIDevice parent_obj;
};

#endif
