/*
 * AMD DRAM Controller Configuration
 *
 * Copyright (c) 2025 Tisenu100
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/qemu-print.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/mem/amd_k8.h"
#include "migration/vmstate.h"

static const VMStateDescription vmstate_amd_dram = {
    .name = "AMD DRAM Controller Configuration",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, AMDDRAMState),
        VMSTATE_END_OF_LIST()
    },
};

static void amd_dram_write_config(PCIDevice *dev, uint32_t address, uint32_t val, int len)
{
    if(address < 0x40) /* Anything below is RO and must be treater this way */
        return;

    qemu_printf("AMD DRAM: dev->pci_conf[0x%02x] = 0x%x\n", address, val);

    pci_default_write_config(dev, address, val, len);
}

static void amd_dram_realize(PCIDevice *pci, Error **errp)
{
    qemu_printf("AMD DRAM: Awake!\n");
}

static void amd_dram_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->realize = amd_dram_realize;
    k->config_write = amd_dram_write_config;
    k->vendor_id = PCI_VENDOR_ID_AMD;
    k->device_id = PCI_DEVICE_ID_AMD_DRAM;
    k->class_id = PCI_CLASS_BRIDGE_HOST;
    dc->user_creatable = false;
    dc->vmsd = &vmstate_amd_dram;
}

static const TypeInfo amd_dram_type_info = {
    .name = TYPE_AMD_DRAM_PCI_DEVICE,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(AMDDRAMState),
    .class_init = amd_dram_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void amd_dram_register_types(void)
{
    type_register_static(&amd_dram_type_info);
}

type_init(amd_dram_register_types)
