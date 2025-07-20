/*
 * VIA K8T800
 *
 * Copyright (c) 2006 Fabrice Bellard
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
#include "qemu/units.h"
#include "qemu/qemu-print.h"
#include "qemu/range.h"
#include "hw/i386/pc.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
#include "hw/pci-host/k8t800.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "qom/object.h"

OBJECT_DECLARE_SIMPLE_TYPE(K8T800State, K8T800_PCI_HOST_BRIDGE)

struct K8T800State {
    PCIHostState parent_obj;

    MemoryRegion *system_memory;
    MemoryRegion *io_memory;
    MemoryRegion *pci_address_space;
    MemoryRegion *ram_memory;
    Range pci_hole;
    uint64_t below_4g_mem_size;
    uint64_t above_4g_mem_size;
    uint64_t pci_hole64_size;
    bool pci_hole64_fix;

    char *pci_type;
};

static void k8t800_realize(PCIDevice *dev, Error **errp)
{
    if (object_property_get_bool(qdev_get_machine(), "iommu", NULL)) {
        warn_report("This PCI Host doesn't support emulated IOMMU");
    }
}

static uint64_t sram_read(void *opaque, hwaddr addr, unsigned size)
{
    PCIK8T800State *d = opaque;
    uint8_t ret = d->sram[addr % 0xff];

    if(!addr) /* Return the SRAM Index instead */
        ret = d->sram_index;

    if(addr)
        qemu_printf("VIA K8T800 SRAM: Reading 0x%02x from address 0x%02x\n", ret, d->sram_index);

    return ret;
}

static void sram_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    PCIK8T800State *d = opaque;

    if(addr)
        d->sram[d->sram_index] = val;
    else
        d->sram_index = val;

    if(addr)
        qemu_printf("VIA K8T800 SRAM: Writing 0x%02x to address 0x%02x\n", (uint8_t)val, d->sram_index);
}

static void sram_remap(PCIK8T800State *s)
{
    PCIDevice *pci_dev = PCI_DEVICE(s);
    uint8_t address = pci_get_byte(pci_dev->config + 0x52);
    bool enabled = (pci_get_byte(pci_dev->config + 0x51) & 1) && (address != 0);

    memory_region_transaction_begin();

    memory_region_set_enabled(&s->sram_io, false);

    if(enabled) {
        memory_region_set_address(&s->sram_io, pci_get_byte(pci_dev->config + 0x52));
        memory_region_set_enabled(&s->sram_io, true);
    }

    memory_region_transaction_commit();

    qemu_printf("VIA K8T800: SRAM was ");
    if(enabled)
        qemu_printf("enabled at address 0x%02x\n", address);
    else
        qemu_printf("disabled\n");
}

static const MemoryRegionOps sram_ops = {
    .read = sram_read,
    .write = sram_write,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void k8t800_update_memory_mappings(PCIK8T800State *f)
{
    PCIDevice *pci_dev = PCI_DEVICE(f);
    uint8_t val;

    memory_region_transaction_begin();

    /* C Segment */
    val = pci_get_byte(pci_dev->config + 0x61);
    for(int i = 0; i < 4; i++) {
        memory_region_set_enabled(&f->shadow_region[i][f->active_state[i]], false);
        f->active_state[i] = (val >> (i * 2)) & 3;
        qemu_printf("VIA K8T800: C%x segment updated to %d\n", i * 4, f->active_state[i]);
        memory_region_set_enabled(&f->shadow_region[i][f->active_state[i]], true);
    }

    /* D Segment */
    val = pci_get_byte(pci_dev->config + 0x62);
    for(int i = 0; i < 4; i++) {
        memory_region_set_enabled(&f->shadow_region[i + 4][f->active_state[i + 4]], false);
        f->active_state[i + 4] = (val >> (i * 2)) & 3;
        qemu_printf("VIA K8T800: D%x segment updated to %d\n", i * 4, f->active_state[i + 4]);
        memory_region_set_enabled(&f->shadow_region[i + 4][f->active_state[i + 4]], true);
    }

    /* E-F Segment and SMRAM */
    val = pci_get_byte(pci_dev->config + 0x63);
    for(int i = 0; i < 2; i++) {
        memory_region_set_enabled(&f->shadow_region[i + 8][f->active_state[i + 8]], false);
        f->active_state[i + 8] = (val >> (4 + (i * 2))) & 3;
        qemu_printf("VIA K8T800: %c segment updated to %d\n", 'F' - i, f->active_state[i + 8]);
        memory_region_set_enabled(&f->shadow_region[i + 8][f->active_state[i + 8]], true);
    }

    /* Qemu doesn't have a clear handling for SMRAM. Treatment happens similarly to non-SMM mode */
    /* How it's treated is to at least give access to the DRAM region when reqeusted so the BIOS can write SMM code on top */
    val = pci_get_byte(pci_dev->config + 0x63) & 3;
    memory_region_set_enabled(&f->low_smram, false);

    qemu_printf("VIA K8T800: SMRAM passing to %s\n", (val != 0) ? "DRAM" : "PCI");
    memory_region_set_enabled(&f->low_smram, val != 0);

    memory_region_transaction_commit();
}


static void k8t800_write_config(PCIDevice *dev, uint32_t address, uint32_t val, int len)
{
    PCIK8T800State *d = K8T800_PCI_DEVICE(dev);

    switch(address) { /* Read Only and Status Registers */
        case 0x40: case 0x41: case 0x43: case 0x44: case 0x49: case 0x4a:
        case 0x58: case 0x59: case 0x5b: case 0x5c: case 0x5d: case 0x5e:
        case 0x5f: case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6d:
        case 0x6e: case 0x6f: case 0x71: case 0xa4: case 0xa5: case 0xa6:
        case 0xa7: case 0x80: case 0x81: case 0x82: case 0x83: case 0xb4:
        case 0xc0: case 0xc1: case 0xc4: case 0xc5: case 0xc8: case 0xc9:
        case 0xca: case 0xcb: case 0xcc: case 0xcd: case 0xd6: case 0xd7:
        return;
    }

    qemu_printf("VIA K8T800: dev->pci_conf[0x%02x] = 0x%x\n", address, val);
    pci_default_write_config(dev, address, val, len);

    switch(address) {
        case 0x61: case 0x62: case 0x63:
            k8t800_update_memory_mappings(d);
        break;
    }
}

static const VMStateDescription vmstate_k8t800 = {
    .name = "VIA K8T800",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, PCIK8T800State),
        VMSTATE_UNUSED(1),
        VMSTATE_END_OF_LIST()
    }
};

static void k8t800_pcihost_get_pci_hole_start(Object *obj, Visitor *v,
                                              const char *name, void *opaque,
                                              Error **errp)
{
    K8T800State *s = K8T800_PCI_HOST_BRIDGE(obj);
    uint64_t val64;
    uint32_t value;

    val64 = range_is_empty(&s->pci_hole) ? 0 : range_lob(&s->pci_hole);
    value = val64;
    assert(value == val64);
    visit_type_uint32(v, name, &value, errp);
}

static void k8t800_pcihost_get_pci_hole_end(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    K8T800State *s = K8T800_PCI_HOST_BRIDGE(obj);
    uint64_t val64;
    uint32_t value;

    val64 = range_is_empty(&s->pci_hole) ? 0 : range_upb(&s->pci_hole) + 1;
    value = val64;
    assert(value == val64);
    visit_type_uint32(v, name, &value, errp);
}

static uint64_t k8t800_pcihost_get_pci_hole64_start_value(Object *obj)
{
    PCIHostState *h = PCI_HOST_BRIDGE(obj);
    K8T800State *s = K8T800_PCI_HOST_BRIDGE(obj);
    Range w64;
    uint64_t value;

    pci_bus_get_w64_range(h->bus, &w64);
    value = range_is_empty(&w64) ? 0 : range_lob(&w64);
    if (!value && s->pci_hole64_fix) {
        value = pc_pci_hole64_start();
    }
    return value;
}

static void k8t800_pcihost_get_pci_hole64_start(Object *obj, Visitor *v,
                                                const char *name,
                                                void *opaque, Error **errp)
{
    uint64_t hole64_start = k8t800_pcihost_get_pci_hole64_start_value(obj);

    visit_type_uint64(v, name, &hole64_start, errp);
}

static void k8t800_pcihost_get_pci_hole64_end(Object *obj, Visitor *v,
                                              const char *name, void *opaque,
                                              Error **errp)
{
    PCIHostState *h = PCI_HOST_BRIDGE(obj);
    K8T800State *s = K8T800_PCI_HOST_BRIDGE(obj);
    uint64_t hole64_start = k8t800_pcihost_get_pci_hole64_start_value(obj);
    Range w64;
    uint64_t value, hole64_end;

    pci_bus_get_w64_range(h->bus, &w64);
    value = range_is_empty(&w64) ? 0 : range_upb(&w64) + 1;
    hole64_end = ROUND_UP(hole64_start + s->pci_hole64_size, 1ULL << 30);
    if (s->pci_hole64_fix && value < hole64_end) {
        value = hole64_end;
    }
    visit_type_uint64(v, name, &value, errp);
}

static void k8t800_reset(DeviceState *dev)
{
    PCIK8T800State *d = K8T800_PCI_DEVICE(dev);
    PCIDevice *pci_dev = PCI_DEVICE(d);

    pci_set_word(pci_dev->config + PCI_COMMAND, PCI_COMMAND_MASTER | PCI_COMMAND_MEMORY);
    pci_set_word(pci_dev->config + PCI_STATUS, PCI_STATUS_CAP_LIST | PCI_STATUS_DEVSEL_MEDIUM);
    pci_set_word(pci_dev->config + 0x10, 0x08);
    pci_set_word(pci_dev->config + PCI_CAPABILITY_LIST, 0xa0);
    pci_set_long(pci_dev->config + 0x41, 0x82808819);
    pci_set_long(pci_dev->config + 0x48, 0x80881918);
    pci_set_word(pci_dev->config + 0x4c, 0x4482);
    pci_set_byte(pci_dev->config + 0x55, 0x08);
    pci_set_byte(pci_dev->config + 0x57, 0x01);
    pci_set_byte(pci_dev->config + 0x58, 0x08);
    pci_set_byte(pci_dev->config + 0x59, 0x68);
    pci_set_byte(pci_dev->config + 0x5b, 0x80);
    pci_set_byte(pci_dev->config + 0x68, 0x01);
    pci_set_byte(pci_dev->config + 0x6a, 0x02);
    pci_set_byte(pci_dev->config + 0x71, 0x48);
    pci_set_byte(pci_dev->config + 0x85, 0x01);
    pci_set_byte(pci_dev->config + 0x86, 0x4f);
    pci_set_long(pci_dev->config + 0xa0, 0x0020c002);
    pci_set_long(pci_dev->config + 0xa4, 0x1f000201);
    pci_set_byte(pci_dev->config + 0xad, 0x02);
    pci_set_byte(pci_dev->config + 0xb0, 0x80);
    pci_set_byte(pci_dev->config + 0xb1, 0x63);
    pci_set_byte(pci_dev->config + 0xb2, 0x08);
    pci_set_long(pci_dev->config + 0xc0, 0x00605808);
    pci_set_long(pci_dev->config + 0xc4, 0x00110020);
    pci_set_long(pci_dev->config + 0xc8, 0x000000d0);
    pci_set_long(pci_dev->config + 0xcc, 0x00350022);
    pci_set_byte(pci_dev->config + 0xde, 0x22);
    pci_set_byte(pci_dev->config + 0xe5, 0xff);
    pci_set_byte(pci_dev->config + 0xe6, 0x01);

    pci_set_byte(pci_dev->config + 0x51, 0x00);
    pci_set_byte(pci_dev->config + 0x52, 0x00);
    sram_remap(d);

    pci_set_byte(pci_dev->config + 0x61, 0x00);
    pci_set_byte(pci_dev->config + 0x62, 0x00);
    pci_set_byte(pci_dev->config + 0x63, 0x00);
    k8t800_update_memory_mappings(d);
}

static void k8t800_pcihost_initfn(Object *obj)
{
    K8T800State *s = K8T800_PCI_HOST_BRIDGE(obj);
    PCIHostState *phb = PCI_HOST_BRIDGE(obj);

    memory_region_init_io(&phb->conf_mem, obj, &pci_host_conf_le_ops, phb, "pci-conf-idx", 4);
    memory_region_init_io(&phb->data_mem, obj, &pci_host_data_le_ops, phb, "pci-conf-data", 4);

    object_property_add_link(obj, PCI_HOST_PROP_RAM_MEM, TYPE_MEMORY_REGION, (Object **) &s->ram_memory, qdev_prop_allow_set_link_before_realize, 0);

    object_property_add_link(obj, PCI_HOST_PROP_PCI_MEM, TYPE_MEMORY_REGION, (Object **) &s->pci_address_space, qdev_prop_allow_set_link_before_realize, 0);

    object_property_add_link(obj, PCI_HOST_PROP_SYSTEM_MEM, TYPE_MEMORY_REGION, (Object **) &s->system_memory, qdev_prop_allow_set_link_before_realize, 0);

    object_property_add_link(obj, PCI_HOST_PROP_IO_MEM, TYPE_MEMORY_REGION, (Object **) &s->io_memory, qdev_prop_allow_set_link_before_realize, 0);
}

static void k8t800_pcihost_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    K8T800State *s = K8T800_PCI_HOST_BRIDGE(dev);
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    qemu_printf("VIA K8T800: Initiating the PCI Bus\n");

    memory_region_add_subregion(s->io_memory, 0xcf8, &phb->conf_mem);
    sysbus_init_ioports(sbd, 0xcf8, 4);

    memory_region_add_subregion(s->io_memory, 0xcfc, &phb->data_mem);
    sysbus_init_ioports(sbd, 0xcfc, 4);

    memory_region_set_flush_coalesced(&phb->data_mem);
    memory_region_add_coalescing(&phb->conf_mem, 0, 4);

    PCIBus *b = pci_root_bus_new(dev, NULL, s->pci_address_space, s->io_memory, 0, TYPE_PCI_BUS);
    phb->bus = b;

    PCIDevice *d = pci_create_simple(b, 0, s->pci_type);
    PCIK8T800State *f = K8T800_PCI_DEVICE(d);

    range_set_bounds(&s->pci_hole, s->below_4g_mem_size, IO_APIC_DEFAULT_ADDRESS - 1);
    pc_pci_as_mapping_init(s->system_memory, s->pci_address_space);

    /* Setup SMRAM */
    qemu_printf("VIA K8T800: Initiating SMRAM\n");
    memory_region_init(&f->smram, OBJECT(d), "smram", 4 * GiB);
    memory_region_set_enabled(&f->smram, true);
    memory_region_init_alias(&f->low_smram, OBJECT(d), "smram-low", s->ram_memory, 0xa0000, 0x20000);
    memory_region_set_enabled(&f->low_smram, false);
    memory_region_add_subregion(&f->smram, 0xa0000, &f->low_smram);

    /* Qemu doesn't have an appropriate SMBASE setup. Per AMD K8 datasheet SMBASE for Hammer processors starts at 0x30000 */
    memory_region_init_alias(&f->smbase, OBJECT(d), "smbase", s->ram_memory, 0x30000, 0x20000);
    memory_region_set_enabled(&f->smbase, true);
    memory_region_add_subregion(&f->smram, 0x30000, &f->smbase);
    object_property_add_const_link(qdev_get_machine(), "smram", OBJECT(&f->smram));

    /* Setup SRAM */
    qemu_printf("VIA K8T800: Setting up SRAM\n");

    for(int i = 0; i < 0xff; i++) /* A memcpy function can be used instead */
        f->sram[i] = 0;

    memory_region_init_io(&f->sram_io, OBJECT(d), &sram_ops, f, "sram", 2);

    /* Setup Shadow RAM */
    qemu_printf("VIA K8T800: Setting up Shadow RAM\n");

    /* Expansion Slots */
    for(int i = 0; i < 8; i++) {
        memory_region_init_alias(&f->shadow_region[i][0], OBJECT(d), "shadow-block-0", s->pci_address_space, 0xc0000 + (i * 0x4000), 0x4000);
        memory_region_add_subregion_overlap(s->system_memory, 0xc0000 + (i * 0x4000), &f->shadow_region[i][0], 1);
        memory_region_set_enabled(&f->shadow_region[i][0], true);

        /* The System doesn't do this! Qemu has no definition of Write Only memory */
        memory_region_init_alias(&f->shadow_region[i][1], OBJECT(d), "shadow-block-1", s->ram_memory, 0xc0000 + (i * 0x4000), 0x4000);
        memory_region_add_subregion_overlap(s->system_memory, 0xc0000 + (i * 0x4000), &f->shadow_region[i][1], 1);
        memory_region_set_enabled(&f->shadow_region[i][1], false);

        memory_region_init_alias(&f->shadow_region[i][2], OBJECT(d), "shadow-block-2", s->ram_memory, 0xc0000 + (i * 0x4000), 0x4000);
        memory_region_add_subregion_overlap(s->system_memory, 0xc0000 + (i * 0x4000), &f->shadow_region[i][2], 1);
        memory_region_set_readonly(&f->shadow_region[i][2], true);
        memory_region_set_enabled(&f->shadow_region[i][2], false);

        memory_region_init_alias(&f->shadow_region[i][3], OBJECT(d), "shadow-block-3", s->ram_memory, 0xc0000 + (i * 0x4000), 0x4000);
        memory_region_add_subregion_overlap(s->system_memory, 0xc0000 + (i * 0x4000), &f->shadow_region[i][3], 1);
        memory_region_set_enabled(&f->shadow_region[i][3], false);
    }

    /* BIOS */
    for(int i = 0; i < 2; i++) {
        memory_region_init_alias(&f->shadow_region[i + 8][0], OBJECT(d), "shadow-block-0", s->pci_address_space, 0xf0000 - (i * 0x10000), 0x10000);
        memory_region_add_subregion_overlap(s->system_memory, 0xf0000 - (i * 0x10000), &f->shadow_region[i + 8][0], 1);
        memory_region_set_enabled(&f->shadow_region[i + 8][0], true);

        /* The System doesn't do this! Qemu has no definition of Write Only memory */
        memory_region_init_alias(&f->shadow_region[i + 8][1], OBJECT(d), "shadow-block-1", s->ram_memory, 0xf0000 - (i * 0x10000), 0x10000);
        memory_region_add_subregion_overlap(s->system_memory, 0xf0000 - (i * 0x10000), &f->shadow_region[i + 8][1], 1);
        memory_region_set_enabled(&f->shadow_region[i + 8][1], false);

        memory_region_init_alias(&f->shadow_region[i + 8][2], OBJECT(d), "shadow-block-2", s->ram_memory, 0xf0000 - (i * 0x10000), 0x10000);
        memory_region_add_subregion_overlap(s->system_memory, 0xf0000 - (i * 0x10000), &f->shadow_region[i + 8][2], 1);
        memory_region_set_readonly(&f->shadow_region[i + 8][2], true);
        memory_region_set_enabled(&f->shadow_region[i + 8][2], false);

        memory_region_init_alias(&f->shadow_region[i + 8][3], OBJECT(d), "shadow-block-3", s->ram_memory, 0xf0000 - (i * 0x10000), 0x10000);
        memory_region_add_subregion_overlap(s->system_memory, 0xf0000 - (i * 0x10000), &f->shadow_region[i + 8][3], 1);
        memory_region_set_enabled(&f->shadow_region[i + 8][3], false);
    }

    /* Clear all active states */
    for(int i = 0; i < 10; i++)
        f->active_state[i] = 0;
}

static void k8t800_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = k8t800_realize;
    device_class_set_legacy_reset(dc, k8t800_reset);
    k->config_write = k8t800_write_config;
    k->vendor_id = PCI_VENDOR_ID_VIA;
    k->device_id = PCI_DEVICE_ID_VIA_K8T800;
    k->revision = 0x02;
    k->class_id = PCI_CLASS_BRIDGE_HOST;
    dc->desc = "VIA K8T800";
    dc->vmsd = &vmstate_k8t800;
    dc->user_creatable = false;
    dc->hotpluggable   = false;
}

static const TypeInfo k8t800_info = {
    .name          = TYPE_K8T800_PCI_DEVICE,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCIK8T800State),
    .class_init    = k8t800_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static const char *k8t800_pcihost_root_bus_path(PCIHostState *host_bridge, PCIBus *rootbus)
{
    return "0000:00";
}

static const Property k8t800_props[] = {
    DEFINE_PROP_SIZE(PCI_HOST_PROP_PCI_HOLE64_SIZE, K8T800State, pci_hole64_size, (1ULL << 31)),
    DEFINE_PROP_SIZE(PCI_HOST_BELOW_4G_MEM_SIZE, K8T800State, below_4g_mem_size, 0),
    DEFINE_PROP_SIZE(PCI_HOST_ABOVE_4G_MEM_SIZE, K8T800State, above_4g_mem_size, 0),
    DEFINE_PROP_BOOL("x-pci-hole64-fix", K8T800State, pci_hole64_fix, true),
    DEFINE_PROP_STRING(K8T800_HOST_PROP_PCI_TYPE, K8T800State, pci_type),
};

static void k8t800_pcihost_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIHostBridgeClass *hc = PCI_HOST_BRIDGE_CLASS(klass);

    hc->root_bus_path = k8t800_pcihost_root_bus_path;
    dc->realize = k8t800_pcihost_realize;
    dc->fw_name = "pci";
    device_class_set_props(dc, k8t800_props);
    /* Reason: needs to be wired up by pc_init1 */
    dc->user_creatable = false;

    object_class_property_add(klass, PCI_HOST_PROP_PCI_HOLE_START, "uint32", k8t800_pcihost_get_pci_hole_start, NULL, NULL, NULL);
    object_class_property_add(klass, PCI_HOST_PROP_PCI_HOLE_END, "uint32", k8t800_pcihost_get_pci_hole_end, NULL, NULL, NULL);
    object_class_property_add(klass, PCI_HOST_PROP_PCI_HOLE64_START, "uint64", k8t800_pcihost_get_pci_hole64_start, NULL, NULL, NULL);
    object_class_property_add(klass, PCI_HOST_PROP_PCI_HOLE64_END, "uint64", k8t800_pcihost_get_pci_hole64_end, NULL, NULL, NULL);
}

static const TypeInfo k8t800_pcihost_info = {
    .name          = TYPE_K8T800_PCI_HOST_BRIDGE,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(K8T800State),
    .instance_init = k8t800_pcihost_initfn,
    .class_init    = k8t800_pcihost_class_init,
};

static void k8t800_register_types(void)
{
    type_register_static(&k8t800_info);
    type_register_static(&k8t800_pcihost_info);
}

type_init(k8t800_register_types)
