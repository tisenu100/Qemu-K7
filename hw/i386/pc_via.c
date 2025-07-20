/*
 * QEMU PC System Emulator
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
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
#include CONFIG_DEVICES

#include "qemu/units.h"
#include "qemu/qemu-print.h"
#include "hw/char/parallel-isa.h"
#include "hw/dma/i8257.h"
#include "hw/loader.h"
#include "hw/i386/x86.h"
#include "hw/i386/pc.h"
#include "hw/i386/apic.h"
#include "hw/pci-host/k8t800.h"
#include "hw/rtc/via_nvram.h"
#include "hw/southbridge/vt8237.h"
#include "hw/display/ramfb.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "hw/usb.h"
#include "net/net.h"
#include "hw/ide/pci.h"
#include "hw/isa/vt82c686.h"
#include "hw/irq.h"
#include "system/kvm.h"
#include "hw/i386/kvm/clock.h"
#include "hw/sysbus.h"
#include "hw/i2c/smbus_eeprom.h"
#include "hw/mem/amd_k8.h"
#include "system/memory.h"
#include "hw/acpi/acpi.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "migration/global_state.h"
#include "migration/misc.h"
#include "system/runstate.h"
#include "target/i386/cpu.h"


static int pc_pci_slot_get_pirq(PCIDevice *isa_bridge_pci, int pci_intx)
{
    int slot_addend;
    slot_addend = PCI_SLOT(isa_bridge_pci->devfn) - 1;
    return (pci_intx + slot_addend) & 3;
}

static void pc_via_init(MachineState *machine)
{
    /* Qemu PC class */
    PCMachineState *pcms = PC_MACHINE(machine);
    PCMachineClass *pcmc = PC_MACHINE_GET_CLASS(pcms);
    X86MachineState *x86ms = X86_MACHINE(machine);

    /* Memory */
    MemoryRegion *system_memory = get_system_memory();
    MemoryRegion *system_io = get_system_io();
    MemoryRegion *ram_memory;
    MemoryRegion *pci_memory = NULL;
    MemoryRegion *rom_memory = system_memory;
    ram_addr_t lowmem;
    uint64_t hole64_size = 0;

    /* AMD Address Mapper */
    PCIDevice *am_pci;
    AMDAMState *am;

    /* PCI & ISA Bus */
    Object *phb = NULL;
    ISABus *isa_bus;

    /* ISA Bridge */
    PCIDevice *isa_bridge_pci;
    DeviceState *isa_bridge;

    /* IDE Controller */
    PCIDevice *ide_pci;

    /* Interrupts */
    qemu_irq smi_irq;
    GSIState *gsi_state;

    qemu_printf("VIA PC: Aweakening!\n");

    qemu_printf("VIA PC: Setting up memory\n");
    ram_memory = machine->ram;
    if (!pcms->max_ram_below_4g) {
        pcms->max_ram_below_4g = 0xe0000000;
    }
    lowmem = pcms->max_ram_below_4g;
    if (machine->ram_size >= pcms->max_ram_below_4g) {
        if (pcmc->gigabyte_align) {
            if (lowmem > 0xc0000000) {
                lowmem = 0xc0000000;
            }
            if (lowmem & (1 * GiB - 1)) {
                warn_report("Large machine and max_ram_below_4g "
                            "(%" PRIu64 ") not a multiple of 1G; "
                            "possible bad performance.",
                            pcms->max_ram_below_4g);
            }
        }
    }

    if (machine->ram_size >= lowmem) {
        x86ms->above_4g_mem_size = machine->ram_size - lowmem;
        x86ms->below_4g_mem_size = lowmem;
    } else {
        x86ms->above_4g_mem_size = 0;
        x86ms->below_4g_mem_size = machine->ram_size;
    }

    qemu_printf("VIA PC: Setting the CPU\n");
    pc_machine_init_sgx_epc(pcms);
    x86_cpus_init(x86ms, pcmc->default_cpu_version);

    if (kvm_enabled()) {
        qemu_printf("VIA PC: KVM Detected! Setting up clock\n");
        kvmclock_create(pcmc->kvmclock_create_always);
    }

    pci_memory = g_new(MemoryRegion, 1);
    memory_region_init(pci_memory, NULL, "pci", UINT64_MAX);
    rom_memory = pci_memory;

    qemu_printf("VIA PC: Setting up the PCI Host\n");
    phb = OBJECT(qdev_new(TYPE_K8T800_PCI_HOST_BRIDGE));
    object_property_add_child(OBJECT(machine), "k8t800", phb);
    object_property_set_link(phb, PCI_HOST_PROP_RAM_MEM, OBJECT(ram_memory), &error_fatal);
    object_property_set_link(phb, PCI_HOST_PROP_PCI_MEM, OBJECT(pci_memory), &error_fatal);
    object_property_set_link(phb, PCI_HOST_PROP_SYSTEM_MEM, OBJECT(system_memory), &error_fatal);
    object_property_set_link(phb, PCI_HOST_PROP_IO_MEM, OBJECT(system_io), &error_fatal);
    object_property_set_uint(phb, PCI_HOST_BELOW_4G_MEM_SIZE, x86ms->below_4g_mem_size, &error_fatal);
    object_property_set_uint(phb, PCI_HOST_ABOVE_4G_MEM_SIZE, x86ms->above_4g_mem_size, &error_fatal);
    object_property_set_str(phb, K8T800_HOST_PROP_PCI_TYPE, TYPE_K8T800_PCI_DEVICE, &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(phb), &error_fatal);

    pcms->pcibus = PCI_BUS(qdev_get_child_bus(DEVICE(phb), "pci.0"));
    pci_bus_map_irqs(pcms->pcibus, pc_pci_slot_get_pirq);
    hole64_size = object_property_get_uint(phb, PCI_HOST_PROP_PCI_HOLE64_SIZE, &error_abort);

    assert(machine->ram_size == x86ms->below_4g_mem_size + x86ms->above_4g_mem_size);
    pc_memory_init(pcms, system_memory, rom_memory, hole64_size);
    gsi_state = pc_gsi_create(&x86ms->gsi, 1);

    qemu_printf("AMD K8: Setting up the Controllers\n");
    pci_create_simple_multifunction(pcms->pcibus, PCI_DEVFN(0x18, 0), TYPE_AMD_HT_PCI_DEVICE);

    am_pci = pci_create_simple(pcms->pcibus, PCI_DEVFN(0x18, 1), TYPE_AMD_AM_PCI_DEVICE);
    am = AMD_AM_PCI_DEVICE(am_pci);
    amd_am_set_smram_region(am, 0xb0); /* Award BIOS use Region 6 for SMM region passthrough */

    pci_create_simple(pcms->pcibus, PCI_DEVFN(0x18, 2), TYPE_AMD_DRAM_PCI_DEVICE);
    pci_create_simple(pcms->pcibus, PCI_DEVFN(0x18, 3), TYPE_AMD_MC_PCI_DEVICE);
    
    qemu_printf("VIA PC: Setting up the ISA Bridge\n");
    isa_bridge_pci = pci_new_multifunction(PCI_DEVFN(0x11, 0x00), TYPE_VT8237_PCI_DEVICE);
    isa_bridge = DEVICE(isa_bridge_pci);

    for (int i = 0; i < IOAPIC_NUM_PINS; i++)
        qdev_connect_gpio_out_named(isa_bridge, "isa-irqs", i, x86ms->gsi[i]);

    pci_realize_and_unref(isa_bridge_pci, pcms->pcibus, &error_fatal);
    isa_bus = ISA_BUS(qdev_get_child_bus(DEVICE(isa_bridge_pci), "isa.0"));

    qemu_printf("VIA PC: Settling NVRAM\n");
    x86ms->rtc = ISA_DEVICE(object_resolve_path_component(OBJECT(isa_bridge_pci), "rtc"));

    qemu_printf("VIA PC: Setting Interrupts\n");
    pc_i8259_create(isa_bus, gsi_state->i8259_irq);
    ioapic_init_gsi(gsi_state, phb);

    if(tcg_enabled()) {
        qemu_printf("VIA PC: TCG Detected! Setting FERR\n");
        x86_register_ferr_irq(x86ms->gsi[13]);
    }

    qemu_printf("VIA PC: Mounting Video\n");
    pc_vga_init(isa_bus, pcms->pcibus);

    /* init basic PC hardware */
    qemu_printf("VIA PC: Initiating Glue Logic\n");
    pc_basic_device_init_clean(pcms, isa_bus, x86ms->gsi, x86ms->rtc, 0, 0x4);

    qemu_printf("VIA PC: Connecting PM\n");
    smi_irq = qemu_allocate_irq(pc_acpi_smi_interrupt, first_cpu, 0);

    qdev_connect_gpio_out_named(DEVICE(isa_bridge_pci), "smi-irq", 0, smi_irq);
    pcms->smbus = I2C_BUS(qdev_get_child_bus(DEVICE(isa_bridge_pci), "i2c"));
    smbus_eeprom_init(pcms->smbus, 4, spd_data_generate(DDR2, machine->ram_size / 4), 0);

    qemu_printf("VIA PC: Starting IDE\n");
    ide_pci = pci_create_simple_multifunction(pcms->pcibus, PCI_DEVFN(0x0f, 1), TYPE_VIA_IDE);
    pci_ide_create_devs(PCI_DEVICE(ide_pci));
    pcms->idebus[0] = qdev_get_child_bus(DEVICE(ide_pci), "ide.0");
    pcms->idebus[1] = qdev_get_child_bus(DEVICE(ide_pci), "ide.1");

    qemu_printf("VIA PC: Passing execution to the BIOS\n");
}

#define DEFINE_VIA_MACHINE(major, minor) \
    DEFINE_PC_VER_MACHINE(pc_via, "pc-via", pc_via_init, false, NULL, major, minor);

#define DEFINE_VIA_MACHINE_AS_LATEST(major, minor) \
    DEFINE_PC_VER_MACHINE(pc_via, "pc-via", pc_via_init, true, "pc-via", major, minor);

static void pc_via_machine_options(MachineClass *m)
{
    PCMachineClass *pcmc = PC_MACHINE_CLASS(m);
    pcmc->pci_root_uid = 0;
    pcmc->default_cpu_version = 1;
    pcmc->has_acpi_build = false;

    m->family = "pc_via";
    m->desc = "Standard PC (K8T800 + VT8237, 2004)";
    m->default_display = "std";
}

static void pc_via_machine_10_1_options(MachineClass *m)
{
    pc_via_machine_options(m);
}

DEFINE_VIA_MACHINE_AS_LATEST(10, 1);
