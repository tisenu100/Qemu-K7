/*
 * AMD Miscellaneous Control Configuration
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

static const VMStateDescription vmstate_amd_mc = {
    .name = "AMD Miscellaneous Control Configuration",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, AMDMCState),
        VMSTATE_END_OF_LIST()
    },
};

static void amd_mc_reset(DeviceState *dev)
{
    PCIDevice *pci_dev = PCI_DEVICE(dev);

    pci_set_long(pci_dev->config + 0x70, 0x51020111);
    pci_set_long(pci_dev->config + 0x74, 0x50008011);
    pci_set_long(pci_dev->config + 0x78, 0x08003800);
    pci_set_long(pci_dev->config + 0x7c, 0x0000221b);
    pci_set_byte(pci_dev->config + 0xe9, 0x01);
}

static void amd_mc_write_config(PCIDevice *dev, uint32_t address, uint32_t val, int len)
{
    if(address < 0x40) /* Anything below is RO and must be treater this way */
        return;

    switch(address) {
        case 0x4a: case 0x4f: case 0x54: case 0xe4:
        case 0xe5: case 0xe6: case 0xe7: case 0xe8:
        case 0xe9: case 0xea: case 0xeb: 
            return;
    }

    qemu_printf("AMD MC: dev->pci_conf[0x%02x] = 0x%x\n", address, val);

    pci_default_write_config(dev, address, val, len);
}

static void amd_mc_realize(PCIDevice *pci, Error **errp)
{
    qemu_printf("AMD MC: Awake!\n");
}

static void amd_mc_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->realize = amd_mc_realize;
    k->config_write = amd_mc_write_config;
    device_class_set_legacy_reset(dc, amd_mc_reset);
    k->vendor_id = PCI_VENDOR_ID_AMD;
    k->device_id = PCI_DEVICE_ID_AMD_MC;
    k->class_id = PCI_CLASS_BRIDGE_HOST;
    dc->user_creatable = false;
    dc->vmsd = &vmstate_amd_mc;
}

static const TypeInfo amd_mc_type_info = {
    .name = TYPE_AMD_MC_PCI_DEVICE,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(AMDMCState),
    .class_init = amd_mc_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void amd_mc_register_types(void)
{
    type_register_static(&amd_mc_type_info);
}

type_init(amd_mc_register_types)
