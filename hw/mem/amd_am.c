/*
 * AMD Address Map Configuration
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

static const VMStateDescription vmstate_amd_am = {
    .name = "AMD Address Map Configuration",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, AMDAMState),
        VMSTATE_END_OF_LIST()
    },
};

void amd_am_set_smram_region(AMDAMState *dev, uint8_t reg)
{
    dev->smram_region_reg = reg;
    qemu_printf("AMD AM: Set SMRAM regio MMIO register to 0x%02x\n", reg);
}

/* This is not how it works. Normally the address mapper asserts memory regions manually which passes them to PCI */
static void amd_am_write_smram_region(AMDAMState *s, uint8_t val)
{
    memory_region_transaction_begin();
    memory_region_set_enabled(&s->smram_region[0], false);
    memory_region_set_enabled(&s->smram_region[1], false);
       
    if(val & 1) {
        if(!(val & 2)) {
            memory_region_set_enabled(&s->smram_region[1], true);
        } else {
            memory_region_set_enabled(&s->smram_region[0], true);
        }

            qemu_printf("AMD AM: Now forwarding MMIO region 6 to PCI\n");            
        }
        memory_region_transaction_commit();
}

static void amd_am_write_config(PCIDevice *dev, uint32_t address, uint32_t val, int len)
{
    AMDAMState *s = AMD_AM_PCI_DEVICE(dev);

    if(address < 0x40) /* Anything below is RO and must be treater this way */
        return;

    qemu_printf("AMD AM: dev->pci_conf[0x%02x] = 0x%x\n", address, val);

    pci_default_write_config(dev, address, val, len);

    if(address == s->smram_region_reg)
        amd_am_write_smram_region(s, val & 0x000f);

}

static void amd_am_realize(PCIDevice *pci, Error **errp)
{
    DeviceState *dev = DEVICE(pci);
    AMDAMState *s = AMD_AM_PCI_DEVICE(dev);
    qemu_printf("AMD AM: Awake!\n");

    qemu_printf("AMD AM: Setting up MMIO Region!\n");
    memory_region_init_alias(&s->smram_region[0], OBJECT(pci), "smram_region", pci_address_space(pci), 0xa0000, 0x20000);
    memory_region_add_subregion_overlap(get_system_memory(), 0xa0000, &s->smram_region[0], 1);
    memory_region_set_enabled(&s->smram_region[0], false);

    memory_region_init_alias(&s->smram_region[1], OBJECT(pci), "smram_region", pci_address_space(pci), 0xa0000, 0x20000);
    memory_region_add_subregion_overlap(get_system_memory(), 0xa0000, &s->smram_region[1], 1);
    memory_region_set_readonly(&s->smram_region[1], true);
    memory_region_set_enabled(&s->smram_region[1], false);

    s->smram_region_reg = 0;
}

static void amd_am_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->realize = amd_am_realize;
    k->config_write = amd_am_write_config;
    k->vendor_id = PCI_VENDOR_ID_AMD;
    k->device_id = PCI_DEVICE_ID_AMD_AM;
    k->class_id = PCI_CLASS_BRIDGE_HOST;
    dc->user_creatable = false;
    dc->vmsd = &vmstate_amd_am;
}

static const TypeInfo amd_am_type_info = {
    .name = TYPE_AMD_AM_PCI_DEVICE,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(AMDAMState),
    .class_init = amd_am_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void amd_am_register_types(void)
{
    type_register_static(&amd_am_type_info);
}

type_init(amd_am_register_types)
