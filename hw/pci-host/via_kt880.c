/*
 * VIA KT880 Desktop North Bridge
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
#include "qemu/range.h"
#include "hw/i386/pc.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
#include "hw/pci-host/via_kt880.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "qom/object.h"

OBJECT_DECLARE_SIMPLE_TYPE(KT880State, KT880_PCI_HOST_BRIDGE)

struct KT880State {
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

static void kt880_register_reset(PCIDevice *d)
{
    /* AMD V-Link */
    pci_default_write_config(d, 0x41, 0x19, 0x01);
    pci_default_write_config(d, 0x42, 0x88, 0x01);
    pci_default_write_config(d, 0x43, 0x8280, 0x02);
    pci_default_write_config(d, 0x45, 0x44, 0x01);
    pci_default_write_config(d, 0x48, 0x18, 0x01);
    pci_default_write_config(d, 0x49, 0x19, 0x01);
    pci_default_write_config(d, 0x4a, 0x88, 0x01);
    pci_default_write_config(d, 0x4b, 0x8280, 0x02);
    pci_default_write_config(d, 0x4d, 0x44, 0x01);

    pci_default_write_config(d, 0x50, 0x08, 0x01);
    pci_default_write_config(d, 0x53, 0x80, 0x01);

    /* DRAM */
    pci_default_write_config(d, 0x56, 0x01, 0x01);
    pci_default_write_config(d, 0x57, 0x01, 0x01);
    pci_default_write_config(d, 0x5a, 0x01, 0x01);
    pci_default_write_config(d, 0x5b, 0x01, 0x01);
    pci_default_write_config(d, 0x5c, 0x01, 0x01);
    pci_default_write_config(d, 0x5d, 0x01, 0x01);
    pci_default_write_config(d, 0x5e, 0x01, 0x01);
    pci_default_write_config(d, 0x5f, 0x01, 0x01);
    pci_default_write_config(d, 0x58, 0x2222, 0x02);

    pci_default_write_config(d, 0x64, 0x02, 0x01);
    pci_default_write_config(d, 0x6b, 0x10, 0x01);

    /* Misc */
    pci_default_write_config(d, 0x71, 0x48, 0x01);

    /* AGP */
    pci_default_write_config(d, 0xa4, 0x1f000201, 0x04);
    pci_default_write_config(d, 0x80, 0x0030c002, 0x04);
    pci_default_write_config(d, 0x84, 0x1f000201, 0x04);

    pci_default_write_config(d, 0xb1, 0x63, 0x01);
    pci_default_write_config(d, 0xb2, 0x08, 0x01);

    /* CPU Interface */
    pci_default_write_config(d, 0xd2, 0x78, 0x01);
    pci_default_write_config(d, 0xdc, 0x07, 0x01);
}

static void kt880_realize(PCIDevice *dev, Error **errp)
{
    kt880_register_reset(d);

    if (object_property_get_bool(qdev_get_machine(), "iommu", NULL)) {
        warn_report("The selected PCI Host doesn't support IOMMU emulation");
    }
}

static void kt880_memory_handler(int reg, PCIKT880State *d)
{
    PCIDevice *pd = PCI_DEVICE(d);
    uint32_t val;

    memory_region_transaction_begin();

    if(!((reg & 2) >> 1)) {
        val = pci_default_read_config(pd, 0x61 + reg, 0x01);

        /*
            C Segment for Register 61h
            D Segment for Register 62h
        */
        for(int i = 0; i < 4; i++) {
            int range = i + (reg * 4);
            memory_region_set_enabled(&pd->shadow_block[pd->block_status[range]][range], false);
            pd->block_status[range] = (val >> (i * 2)) & 3;
            memory_region_set_enabled(&pd->shadow_block[pd->block_status[range]][range], true);
        }
    } else {
        /* E Segment */
        val = pci_default_read_config(pd, 0x63, 0x01);
        memory_region_set_enabled(&pd->shadow_block[pd->block_status[8]][8], false);
        pd->block_status[8] = (val >> 4) & 3;
        memory_region_set_enabled(&pd->shadow_block[pd->block_status[8]][8], true);

        /* F Segment */
        memory_region_set_enabled(&pd->shadow_block[pd->block_status[9]][9], false);
        pd->block_status[9] = (val >> 6) & 3;
        memory_region_set_enabled(&pd->shadow_block[pd->block_status[9]][9], true);


        /*
            SMRAM

            Bit 1: Expose to PCI
            Bit 0: Expose to DRAM
        */
        memory_region_set_enabled(&pd->smram_region, false);
        memory_region_set_enabled(&pd->low_smram, false);

        memory_region_set_enabled(&pd->low_smram, val & 1);
        memory_region_set_enabled(&pd->smram_region, !!(val & 2));
    }

    memory_region_transaction_commit();
}


static void kt880_write_config(PCIDevice *dev, uint32_t address, uint32_t val, int len)
{
    PCIKT880State *d = KT880_PCI_DEVICE(dev);

    pci_default_write_config(dev, address, val, len);

    if (ranges_overlap(address, len, 0x61, 0x03))
        kt880_memory_handler(address - 0x61, d);
}

static int kt880_post_load(void *opaque, int version_id)
{
    PCIKT880State *d = opaque;

    /* Restore Memory Mappings */
    kt880_memory_handler(0, d);
    kt880_memory_handler(1, d);
    kt880_memory_handler(2, d);
    return 0;
}

static const VMStateDescription vmstate_kt880 = {
    .name = "VIA KT880 Desktop North Bridge",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = kt880_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, PCIKT880State),
        VMSTATE_UNUSED(1),
        VMSTATE_END_OF_LIST()
    }
};

static void kt880_pcihost_get_pci_hole_start(Object *obj, Visitor *v,
                                              const char *name, void *opaque,
                                              Error **errp)
{
    KT880State *s = KT880_PCI_HOST_BRIDGE(obj);
    uint64_t val64;
    uint32_t value;

    val64 = range_is_empty(&s->pci_hole) ? 0 : range_lob(&s->pci_hole);
    value = val64;
    assert(value == val64);
    visit_type_uint32(v, name, &value, errp);
}

static void kt880_pcihost_get_pci_hole_end(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    KT880State *s = KT880_PCI_HOST_BRIDGE(obj);
    uint64_t val64;
    uint32_t value;

    val64 = range_is_empty(&s->pci_hole) ? 0 : range_upb(&s->pci_hole) + 1;
    value = val64;
    assert(value == val64);
    visit_type_uint32(v, name, &value, errp);
}

static uint64_t kt880_pcihost_get_pci_hole64_start_value(Object *obj)
{
    PCIHostState *h = PCI_HOST_BRIDGE(obj);
    KT880State *s = KT880_PCI_HOST_BRIDGE(obj);
    Range w64;
    uint64_t value;

    pci_bus_get_w64_range(h->bus, &w64);
    value = range_is_empty(&w64) ? 0 : range_lob(&w64);
    if (!value && s->pci_hole64_fix) {
        value = pc_pci_hole64_start();
    }
    return value;
}

static void kt880_pcihost_get_pci_hole64_start(Object *obj, Visitor *v,
                                                const char *name,
                                                void *opaque, Error **errp)
{
    uint64_t hole64_start = kt880_pcihost_get_pci_hole64_start_value(obj);

    visit_type_uint64(v, name, &hole64_start, errp);
}

static void kt880_pcihost_get_pci_hole64_end(Object *obj, Visitor *v,
                                              const char *name, void *opaque,
                                              Error **errp)
{
    PCIHostState *h = PCI_HOST_BRIDGE(obj);
    KT880State *s = KT880_PCI_HOST_BRIDGE(obj);
    uint64_t hole64_start = kt880_pcihost_get_pci_hole64_start_value(obj);
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

static void kt880_pcihost_initfn(Object *obj)
{
    KT880State *s = KT880_PCI_HOST_BRIDGE(obj);
    PCIHostState *phb = PCI_HOST_BRIDGE(obj);

    memory_region_init_io(&phb->conf_mem, obj, &pci_host_conf_le_ops, phb, "pci-conf-idx", 4);
    memory_region_init_io(&phb->data_mem, obj, &pci_host_data_le_ops, phb, "pci-conf-data", 4);

    /* Memory Regions */
    /* DRAM */
    object_property_add_link(obj, PCI_HOST_PROP_RAM_MEM, TYPE_MEMORY_REGION, (Object **) &s->ram_memory, qdev_prop_allow_set_link_before_realize, 0);

    /* PCI */
    object_property_add_link(obj, PCI_HOST_PROP_PCI_MEM, TYPE_MEMORY_REGION, (Object **) &s->pci_address_space, qdev_prop_allow_set_link_before_realize, 0);

    /* System */
    object_property_add_link(obj, PCI_HOST_PROP_SYSTEM_MEM, TYPE_MEMORY_REGION, (Object **) &s->system_memory, qdev_prop_allow_set_link_before_realize, 0);

    /* IO */
    object_property_add_link(obj, PCI_HOST_PROP_IO_MEM, TYPE_MEMORY_REGION, (Object **) &s->io_memory, qdev_prop_allow_set_link_before_realize, 0);
}

static void kt880_pcihost_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    KT880State *s = KT880_PCI_HOST_BRIDGE(dev);
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    PCIBus *b;
    PCIDevice *d;
    PCIKT880State *f;

    b = pci_root_bus_new(dev, NULL, s->pci_address_space, s->io_memory, 0, TYPE_PCI_BUS);
    phb->bus = b;
    d = pci_create_simple(b, 0, s->pci_type);
    f = KT880_PCI_DEVICE(d);

    memory_region_add_subregion(s->io_memory, 0xcf8, &phb->conf_mem);
    sysbus_init_ioports(sbd, 0xcf8, 4);

    memory_region_add_subregion(s->io_memory, 0xcfc, &phb->data_mem);
    sysbus_init_ioports(sbd, 0xcfc, 4);

    /* Register KT880 0xcf8 port as coalesced pio */
    memory_region_set_flush_coalesced(&phb->data_mem);
    memory_region_add_coalescing(&phb->conf_mem, 0, 4);

    /* IO APIC */
    range_set_bounds(&s->pci_hole, s->below_4g_mem_size, IO_APIC_DEFAULT_ADDRESS - 1);

    /* Setup PCI Memory */
    pc_pci_as_mapping_init(s->system_memory, s->pci_address_space);

    /* if *disabled* show SMRAM to all CPUs */

    /*
        smram_region: SMRAM -> PCI
        low_smram: SMRAM -> DRAM
        smram: SMRAM on SMM mode
    */
    memory_region_init_alias(&f->smram_region, OBJECT(d), "smram-region", s->pci_address_space, 0xa0000, 0x20000);
    memory_region_add_subregion_overlap(s->system_memory, 0xa0000, &f->smram_region, 1);
    memory_region_set_enabled(&f->smram_region, true);
    memory_region_init(&f->smram, OBJECT(d), "smram", 4 * GiB);
    memory_region_set_enabled(&f->smram, true);
    memory_region_init_alias(&f->low_smram, OBJECT(d), "smram-low", s->ram_memory, 0xa0000, 0x20000);
    memory_region_set_enabled(&f->low_smram, true);
    memory_region_add_subregion(&f->smram, 0xa0000, &f->low_smram);
    object_property_add_const_link(qdev_get_machine(), "smram", OBJECT(&f->smram));


    /* Setup Shadow RAM */

    /*
       VIA Datasheet points to these modes per register:
       0: Disabled
       1: Write Enable
       2: Read Enable
       3: R/W Enable

       Note: for State 1. Qemu doesn't have a definition of write only memory. The RW status can be used just fine but that ain't correct
    */

    /* PCI Space 0xc0000-0xdffff */
    for(int i = 0; i < 8; i++) {
        memory_region_init_alias(&f->shadow_blocks[0][i], OBJECT(d), "shadow-block", s->pci_address_space, 0xc0000 + (i * 0x4000), 0x4000);
        memory_region_init_alias(&f->shadow_blocks[1][i], OBJECT(d), "shadow-block-w", s->ram_memory, 0xc0000 + (i * 0x4000), 0x4000);
        memory_region_init_alias(&f->shadow_blocks[2][i], OBJECT(d), "shadow-block-r", s->ram_memory, 0xc0000 + (i * 0x4000), 0x4000);
        memory_region_set_readonly(&f->shadow_blocks[2][i], true);
        memory_region_init_alias(&f->shadow_blocks[3][i], OBJECT(d), "shadow-block-rw", s->ram_memory, 0xc0000 + (i * 0x4000), 0x4000);

        for(int j = 0; j < 3; j++){
            memory_region_set_enabled(&f->shadow_blocks[j][i], false);
            memory_region_add_subregion_overlap(s->system_memory, 0xc0000 + (i * 0x4000), &f->shadow_blocks[j][i], 1);
        }
    }

    /* System Space */
    memory_region_init_alias(&f->shadow_blocks[0][8], OBJECT(d), "shadow-block", s->pci_address_space, 0xe0000, 0x10000);
    memory_region_init_alias(&f->shadow_blocks[1][8], OBJECT(d), "shadow-block-w", s->ram_memory, 0xe0000, 0x10000);
    memory_region_init_alias(&f->shadow_blocks[2][8], OBJECT(d), "shadow-block-r", s->ram_memory, 0xe0000, 0x10000);
    memory_region_set_readonly(&f->shadow_blocks[2][8], true);
    memory_region_init_alias(&f->shadow_blocks[3][8], OBJECT(d), "shadow-block-rw", s->ram_memory, 0xe0000, 0x10000);

    for(int j = 0; j < 3; j++){
        memory_region_set_enabled(&f->shadow_blocks[j][i], false);
        memory_region_add_subregion_overlap(s->system_memory, 0xe0000, &f->shadow_blocks[j][8], 1);
    }

    memory_region_init_alias(&f->shadow_blocks[0][9], OBJECT(d), "shadow-block", s->pci_address_space, 0xf0000, 0x10000);
    memory_region_init_alias(&f->shadow_blocks[1][9], OBJECT(d), "shadow-block-w", s->ram_memory, 0xf0000, 0x10000);
    memory_region_init_alias(&f->shadow_blocks[2][9], OBJECT(d), "shadow-block-r", s->ram_memory, 0xf0000, 0x10000);
    memory_region_set_readonly(&f->shadow_blocks[2][9], true);
    memory_region_init_alias(&f->shadow_blocks[3][9], OBJECT(d), "shadow-block-rw", s->ram_memory, 0xf0000, 0x10000);

    for(int j = 0; j < 3; j++){
        memory_region_set_enabled(&f->shadow_blocks[j][i], false);
        memory_region_add_subregion_overlap(s->system_memory, 0xf0000, &f->shadow_blocks[j][8], 1);
    }

    /* Initiate Basic Registers and setup all Configurations appropriately */
    kt880_register_reset(d);
    kt880_memory_handler(0, f);
    kt880_memory_handler(1, f);
    kt880_memory_handler(2, f);
}

static void kt880_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = kt880_realize;
    k->config_write = kt880_write_config;
    k->vendor_id = PCI_VENDOR_ID_VIA;
    k->device_id = PCI_DEVICE_ID_VIA_KT880;
    k->revision = 0x01;
    k->class_id = PCI_CLASS_BRIDGE_HOST;
    dc->desc = "VIA KT880 Desktop North Bridge";
    dc->vmsd = &vmstate_kt880;
    dc->user_creatable = false;
    dc->hotpluggable   = false;
}

static const TypeInfo kt880_info = {
    .name          = TYPE_KT880_PCI_DEVICE,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCIKT880State),
    .class_init    = kt880_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static const char *kt880_pcihost_root_bus_path(PCIHostState *host_bridge,
                                                PCIBus *rootbus)
{
    return "0000:00";
}

static const Property kt880_props[] = {
    DEFINE_PROP_SIZE(PCI_HOST_PROP_PCI_HOLE64_SIZE, KT880State,
                     pci_hole64_size, (1ULL << 31)),
    DEFINE_PROP_SIZE(PCI_HOST_BELOW_4G_MEM_SIZE, KT880State,
                     below_4g_mem_size, 0),
    DEFINE_PROP_SIZE(PCI_HOST_ABOVE_4G_MEM_SIZE, KT880State,
                     above_4g_mem_size, 0),
    DEFINE_PROP_BOOL("x-pci-hole64-fix", KT880State, pci_hole64_fix, true),
    DEFINE_PROP_STRING(KT880_HOST_PROP_PCI_TYPE, KT880State, pci_type),
};

static void kt880_pcihost_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIHostBridgeClass *hc = PCI_HOST_BRIDGE_CLASS(klass);

    hc->root_bus_path = kt880_pcihost_root_bus_path;
    dc->realize = kt880_pcihost_realize;
    dc->fw_name = "pci";
    device_class_set_props(dc, kt880_props);
    /* Reason: needs to be wired up by pc_init1 */
    dc->user_creatable = false;

    object_class_property_add(klass, PCI_HOST_PROP_PCI_HOLE_START, "uint32",
                              kt880_pcihost_get_pci_hole_start,
                              NULL, NULL, NULL);

    object_class_property_add(klass, PCI_HOST_PROP_PCI_HOLE_END, "uint32",
                              kt880_pcihost_get_pci_hole_end,
                              NULL, NULL, NULL);

    object_class_property_add(klass, PCI_HOST_PROP_PCI_HOLE64_START, "uint64",
                              kt880_pcihost_get_pci_hole64_start,
                              NULL, NULL, NULL);

    object_class_property_add(klass, PCI_HOST_PROP_PCI_HOLE64_END, "uint64",
                              kt880_pcihost_get_pci_hole64_end,
                              NULL, NULL, NULL);
}

static const TypeInfo kt880_pcihost_info = {
    .name          = TYPE_KT880_PCI_HOST_BRIDGE,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(KT880State),
    .instance_init = kt880_pcihost_initfn,
    .class_init    = kt880_pcihost_class_init,
};

static void kt880_register_types(void)
{
    type_register_static(&kt880_info);
    type_register_static(&kt880_pcihost_info);
}

type_init(kt880_register_types)
