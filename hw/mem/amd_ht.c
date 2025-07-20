/*
 * AMD HyperTransport Technology Configuration
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

static const VMStateDescription vmstate_amd_ht = {
    .name = "AMD HyperTransport Technology Configuration",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, AMDHTState),
        VMSTATE_END_OF_LIST()
    },
};

static void amd_ht_reset(DeviceState *dev)
{
    PCIDevice *pci_dev = PCI_DEVICE(dev);

    pci_set_long(pci_dev->config + 0x40, 0x00010101);
    pci_set_long(pci_dev->config + 0x44, 0x00010101);
    pci_set_long(pci_dev->config + 0x48, 0x00010101);
    pci_set_long(pci_dev->config + 0x4c, 0x00010101);
    pci_set_long(pci_dev->config + 0x50, 0x00010101);
    pci_set_long(pci_dev->config + 0x54, 0x00010101);
    pci_set_long(pci_dev->config + 0x58, 0x00010101);
    pci_set_long(pci_dev->config + 0x5c, 0x00010101);
    pci_set_long(pci_dev->config + 0x64, 0x000000e4);
    pci_set_long(pci_dev->config + 0x68, 0x0f000000);
    pci_set_long(pci_dev->config + 0x84, 0x00110000);
    pci_set_long(pci_dev->config + 0x8c, 0x00000002);
    pci_set_long(pci_dev->config + 0xa4, 0x00110000);
    pci_set_long(pci_dev->config + 0xac, 0x00000002);
    pci_set_long(pci_dev->config + 0xc4, 0x00110000);
    pci_set_long(pci_dev->config + 0xcc, 0x00000002);
    pci_set_long(pci_dev->config + 0xe4, 0x00110000);
    pci_set_long(pci_dev->config + 0xec, 0x00000002);

    /* Specify that the Link is connected */
    pci_set_byte(pci_dev->config + 0x98, 0x10); /* LDN0 */
    pci_set_byte(pci_dev->config + 0xb8, 0x10); /* LDN1 */
    pci_set_byte(pci_dev->config + 0xd8, 0x10); /* LDN2 */

    /* Link Frequency */
    pci_set_word(pci_dev->config + 0x8a, 0x7ff5);
    pci_set_word(pci_dev->config + 0xaa, 0x7ff5);
    pci_set_word(pci_dev->config + 0xca, 0x7ff5);
    pci_set_word(pci_dev->config + 0xea, 0x7ff5);
}

static uint32_t amd_ht_read_config(PCIDevice *dev, uint32_t address, int len)
{
    uint32_t ret;

    ret = pci_default_read_config(dev, address, len);

    /* Simulate a pending and successful connection to LDN0 */
    switch(address) {
        case 0x98:
            pci_set_byte(dev->config + address, 0x01);
        break;

        case 0xb8: case 0xd8:
            pci_set_byte(dev->config + address, 0x00);
        break;
    }

    return ret;
}

static void amd_ht_write_config(PCIDevice *dev, uint32_t address, uint32_t val, int len)
{
    if(address < 0x40) /* Anything below is RO and must be treated this way */
        return;

    switch(address) {
        case 0x80: case 0x81: case 0x82: case 0x83:
        case 0x8c: case 0x8d: case 0x8e: case 0x8f:
        case 0x98: case 0x99: case 0x9a: case 0x9b:
        case 0xa0: case 0xa1: case 0xa2: case 0xa3:
        case 0xac: case 0xad: case 0xae: case 0xaf:
        case 0xb8: case 0xb9: case 0xba: case 0xbb:
        case 0xc0: case 0xc1: case 0xc2: case 0xc3:
        case 0xcc: case 0xcd: case 0xce: case 0xcf:
        case 0xd8: case 0xd9: case 0xda: case 0xdb:
        case 0xec: case 0xed: case 0xee: case 0xef:
            return;
    }

    qemu_printf("AMD HT: dev->pci_conf[0x%02x] = 0x%x\n", address, val);

    pci_default_write_config(dev, address, val, len);
}

static void amd_ht_realize(PCIDevice *pci, Error **errp)
{
    qemu_printf("AMD HT: Awake!\n");
}

static void amd_ht_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->realize = amd_ht_realize;
    k->config_write = amd_ht_write_config;
    k->config_read = amd_ht_read_config;
    device_class_set_legacy_reset(dc, amd_ht_reset);
    k->vendor_id = PCI_VENDOR_ID_AMD;
    k->device_id = PCI_DEVICE_ID_AMD_HT;
    k->class_id = PCI_CLASS_BRIDGE_HOST;
    dc->user_creatable = false;
    dc->vmsd = &vmstate_amd_ht;
}

static const TypeInfo amd_ht_type_info = {
    .name = TYPE_AMD_HT_PCI_DEVICE,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(AMDHTState),
    .class_init = amd_ht_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void amd_ht_register_types(void)
{
    type_register_static(&amd_ht_type_info);
}

type_init(amd_ht_register_types)
