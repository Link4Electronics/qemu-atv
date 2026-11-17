/*
 * QEMU PC System Emulator
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
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
#include "hw/char/parallel-isa.h"
#include "hw/i386/x86.h"
#include "hw/i386/pc.h"
#include "hw/i386/apic.h"
#include "hw/pci-host/i440fx.h"
#include "hw/southbridge/piix.h"
#include "hw/display/ramfb.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "qom/object.h"
#include "qemu/module.h"
#include "qemu/bitops.h"
#include "migration/vmstate.h"
#include "hw/usb/usb.h"
#include "net/net.h"
#include "qemu/timer.h"
#include "system/memory.h"
#include "exec/memattrs.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "ui/vgafont.h"
#include "hw/ide/pci.h"
#include "hw/ide/ahci-pci.h"
#include "hw/ide/ahci.h"
#include "hw/ide/ide-dev.h"
#include "hw/core/irq.h"
#include "system/kvm.h"
#include "hw/i386/kvm/clock.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/smbus_eeprom.h"
#include "system/memory.h"

#define TYPE_ATV_NVIDIA_GPU "atv-nvidia-gpu"
#include "hw/acpi/acpi.h"
#include "hw/vfio/types.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "system/xen.h"
#ifdef CONFIG_XEN
#include <xen/hvm/hvm_info_table.h>
#include "hw/xen/xen_pt.h"
#include "hw/xen/xen_igd.h"
#endif
#include "hw/xen/xen-x86.h"
#include "hw/xen/xen.h"
#include "migration/global_state.h"
#include "migration/misc.h"
#include "system/runstate.h"
#include "system/numa.h"
#include "hw/hyperv/vmbus-bridge.h"
#include "hw/mem/nvdimm.h"
#include "hw/uefi/var-service-api.h"
#include "hw/i386/acpi-build.h"
#include "target/i386/cpu.h"

#define XEN_IOAPIC_NUM_PIRQS 128ULL

static GlobalProperty pc_piix_compat_defaults[] = {
    { TYPE_RAMFB_DEVICE, "use-legacy-x86-rom", "true" },
    { TYPE_VFIO_PCI_NOHOTPLUG, "use-legacy-x86-rom", "true" },
};
static const size_t pc_piix_compat_defaults_len =
    G_N_ELEMENTS(pc_piix_compat_defaults);

/*
 * Return the global irq number corresponding to a given device irq
 * pin. We could also use the bus number to have a more precise mapping.
 */
static int pc_pci_slot_get_pirq(PCIDevice *pci_dev, int pci_intx)
{
    int slot_addend;
    slot_addend = PCI_SLOT(pci_dev->devfn) - 1;
    return (pci_intx + slot_addend) & 3;
}

static void piix_intx_routing_notifier_xen(PCIDevice *dev)
{
    int i;

    /* Scan for updates to PCI link routes. */
    for (i = 0; i < PIIX_NUM_PIRQS; i++) {
        const PCIINTxRoute route = pci_device_route_intx_to_irq(dev, i);
        const uint8_t v = route.mode == PCI_INTX_ENABLED ? route.irq : 0;
        xen_set_pci_link_route(i, v);
    }
}

/* PC hardware initialisation */
static void pc_init1(MachineState *machine, const char *pci_type)
{
    PCMachineState *pcms = PC_MACHINE(machine);
    PCMachineClass *pcmc = PC_MACHINE_GET_CLASS(pcms);
    X86MachineState *x86ms = X86_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    MemoryRegion *system_io = get_system_io();
    Object *phb;
    ISABus *isa_bus;
    Object *piix4_pm = NULL;
    qemu_irq smi_irq;
    GSIState *gsi_state;
    MemoryRegion *ram_memory;
    MemoryRegion *pci_memory = NULL;
    ram_addr_t lowmem;
    uint64_t hole64_size = 0;
    PCIDevice *pci_dev;
    DeviceState *dev;
    size_t i;

    assert(pcmc->pci_enabled);

    /*
     * Calculate ram split, for memory below and above 4G.  It's a bit
     * complicated for backward compatibility reasons ...
     *
     *  - Traditional split is 3.5G (lowmem = 0xe0000000).  This is the
     *    default value for max_ram_below_4g now.
     *
     *  - Then, to gigabyte align the memory, we move the split to 3G
     *    (lowmem = 0xc0000000).  But only in case we have to split in
     *    the first place, i.e. ram_size is larger than (traditional)
     *    lowmem.  And for new machine types (gigabyte_align = true)
     *    only, for live migration compatibility reasons.
     *
     *  - Next the max-ram-below-4g option was added, which allowed to
     *    reduce lowmem to a smaller value, to allow a larger PCI I/O
     *    window below 4G.  qemu doesn't enforce gigabyte alignment here,
     *    but prints a warning.
     *
     *  - Finally max-ram-below-4g got updated to also allow raising lowmem,
     *    so legacy non-PAE guests can get as much memory as possible in
     *    the 32bit address space below 4G.
     *
     *  - Note that Xen has its own ram setup code in xen_ram_init(),
     *    called via xen_hvm_init_pc().
     *
     * Examples:
     *    qemu -M pc-1.7 -m 4G    (old default)    -> 3584M low,  512M high
     *    qemu -M pc -m 4G        (new default)    -> 3072M low, 1024M high
     *    qemu -M pc,max-ram-below-4g=2G -m 4G     -> 2048M low, 2048M high
     *    qemu -M pc,max-ram-below-4g=4G -m 3968M  -> 3968M low (=4G-128M)
     */
    if (xen_enabled()) {
        xen_hvm_init_pc(pcms, &ram_memory);
    } else {
        ram_memory = machine->ram;
        if (!pcms->max_ram_below_4g) {
            pcms->max_ram_below_4g = 0xe0000000; /* default: 3.5G */
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
    }

    pc_machine_init_sgx_epc(pcms);
    x86_cpus_init(x86ms, pcmc->default_cpu_version);

    if (kvm_enabled()) {
        kvmclock_create(pcmc->kvmclock_create_always);
    }

    pci_memory = g_new(MemoryRegion, 1);
    memory_region_init(pci_memory, NULL, "pci", UINT64_MAX);

    phb = OBJECT(qdev_new(TYPE_I440FX_PCI_HOST_BRIDGE));
    object_property_add_child(OBJECT(machine), "i440fx", phb);
    object_property_set_link(phb, PCI_HOST_PROP_RAM_MEM,
                             OBJECT(ram_memory), &error_fatal);
    object_property_set_link(phb, PCI_HOST_PROP_PCI_MEM,
                             OBJECT(pci_memory), &error_fatal);
    object_property_set_link(phb, PCI_HOST_PROP_SYSTEM_MEM,
                             OBJECT(system_memory), &error_fatal);
    object_property_set_link(phb, PCI_HOST_PROP_IO_MEM,
                             OBJECT(system_io), &error_fatal);
    object_property_set_uint(phb, PCI_HOST_BELOW_4G_MEM_SIZE,
                             x86ms->below_4g_mem_size, &error_fatal);
    object_property_set_uint(phb, PCI_HOST_ABOVE_4G_MEM_SIZE,
                             x86ms->above_4g_mem_size, &error_fatal);
    object_property_set_str(phb, I440FX_HOST_PROP_PCI_TYPE, pci_type,
                            &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(phb), &error_fatal);

    pcms->pcibus = PCI_BUS(qdev_get_child_bus(DEVICE(phb), "pci.0"));
    pci_bus_map_irqs(pcms->pcibus,
                     xen_enabled() ? xen_pci_slot_get_pirq
                                   : pc_pci_slot_get_pirq);

    hole64_size = object_property_get_uint(phb,
                                           PCI_HOST_PROP_PCI_HOLE64_SIZE,
                                           &error_abort);

    /* allocate ram and load rom/bios */
    if (!xen_enabled()) {
        pc_memory_init(pcms, system_memory, pci_memory, hole64_size);
    } else {
        assert(machine->ram_size == x86ms->below_4g_mem_size +
                                    x86ms->above_4g_mem_size);

        pc_system_flash_cleanup_unused(pcms);
        if (machine->kernel_filename != NULL) {
            /* For xen HVM direct kernel boot, load linux here */
            xen_load_linux(pcms);
        }
    }

    gsi_state = pc_gsi_create(&x86ms->gsi, true);

    pci_dev = pci_new_multifunction(PCI_DEVFN(0x1f, 0), pcms->south_bridge);
    /* ATV: the real ATV1 has no function at 00:1f.2 (its USB is the ICH7 UHCI
     * at 00:1d.0, created below), so keep the PIIX3 UHCI disabled; the IDE
     * child at 00:1f.1 is what carries the PATA disk. */
    object_property_set_bool(OBJECT(pci_dev), "has-usb",
                             pcms->car_size > 0 ? false : machine_usb(machine),
                             &error_abort);
    object_property_set_bool(OBJECT(pci_dev), "has-acpi",
                             x86_machine_is_acpi_enabled(x86ms),
                             &error_abort);
    object_property_set_bool(OBJECT(pci_dev), "has-pic", false,
                             &error_abort);
    object_property_set_bool(OBJECT(pci_dev), "has-pit", false,
                             &error_abort);
    qdev_prop_set_uint32(DEVICE(pci_dev), "smb_io_base", 0xb100);
    object_property_set_bool(OBJECT(pci_dev), "smm-enabled",
                             x86_machine_is_smm_enabled(x86ms),
                             &error_abort);
    dev = DEVICE(pci_dev);
    for (i = 0; i < ISA_NUM_IRQS; i++) {
        qdev_connect_gpio_out_named(dev, "isa-irqs", i, x86ms->gsi[i]);
    }
    pci_realize_and_unref(pci_dev, pcms->pcibus, &error_fatal);

    if (xen_enabled()) {
        pci_device_set_intx_routing_notifier(
                    pci_dev, piix_intx_routing_notifier_xen);

        /*
         * Xen supports additional interrupt routes from the PCI devices to
         * the IOAPIC: the four pins of each PCI device on the bus are also
         * connected to the IOAPIC directly.
         * These additional routes can be discovered through ACPI.
         */
        pci_bus_irqs(pcms->pcibus, xen_intx_set_irq, pci_dev,
                     XEN_IOAPIC_NUM_PIRQS);
    }

    isa_bus = ISA_BUS(qdev_get_child_bus(DEVICE(pci_dev), "isa.0"));
    x86ms->rtc = ISA_DEVICE(object_resolve_path_component(OBJECT(pci_dev),
                                                          "rtc"));
    piix4_pm = object_resolve_path_component(OBJECT(pci_dev), "pm");
    dev = DEVICE(object_resolve_path_component(OBJECT(pci_dev), "ide"));
    pci_ide_create_devs(PCI_DEVICE(dev));
    pcms->idebus[0] = qdev_get_child_bus(dev, "ide.0");
    pcms->idebus[1] = qdev_get_child_bus(dev, "ide.1");


    if (x86ms->pic == ON_OFF_AUTO_ON || x86ms->pic == ON_OFF_AUTO_AUTO) {
        pc_i8259_create(isa_bus, gsi_state->i8259_irq);
    }

    ioapic_init_gsi(gsi_state, phb);

    if (tcg_enabled()) {
        x86_register_ferr_irq(x86ms->gsi[13]);
    }

    if (pcms->car_size > 0) {
        PCIDevice *uhci;
        DeviceState *uhci_dev;

        pci_create_simple(pcms->pcibus, -1, TYPE_ATV_NVIDIA_GPU);

        /*
         * Real ATV1 (lspci, atv-devicetree/pci/vv-xxx-all.txt):
         *   00:1d.0  8086:27c8  ICH7 UHCI (IR receiver 05ac:8241 hangs here)
         *   00:1f.1  8086:27df  ICH7 IDE, prog-if 8f  <- the PATA disk
         * There is no SATA/AHCI function and the firmware's USB stack is
         * UHCI-only (FvFile 2fb92efa "Usb Uhci Driver"), so the controller
         * must be a UHCI at 00:1d.0.  pcms->idebus[] keeps pointing at the
         * ICH7 IDE child so -drive if=ide / -device ide-hd,bus=ide.0 land on
         * the controller the firmware's "PCI ATA/ATAPI Bus Driver"
         * (FvFile 69fd8e47) actually binds.
         */
        uhci_dev = DEVICE(pci_create_simple_multifunction(pcms->pcibus,
                                PCI_DEVFN(0x1d, 0), TYPE_ICH9_USB_UHCI(1)));
        uhci = PCI_DEVICE(uhci_dev);
        pci_config_set_vendor_id(uhci->config, PCI_VENDOR_ID_INTEL);
        pci_config_set_device_id(uhci->config, 0x27c8);
        pci_config_set_revision(uhci->config, 0x02);
        {
            USBBus *atv_usb;
            atv_usb = USB_BUS(object_resolve_type_unambiguous(
                                  TYPE_USB_BUS, &error_abort));
            usb_create_simple(atv_usb, "atv-ir");
        }
    } else {
        pc_vga_init(isa_bus, pcms->pcibus);
    }

    /* init basic PC hardware */
    pc_basic_device_init(pcms, isa_bus, x86ms->gsi, x86ms->rtc,
                         !MACHINE_CLASS(pcmc)->no_floppy, 0x4);

    pc_nic_init(pcmc, isa_bus, pcms->pcibus);

    if (piix4_pm) {
        smi_irq = qemu_allocate_irq(pc_acpi_smi_interrupt, first_cpu, 0);

        qdev_connect_gpio_out_named(DEVICE(piix4_pm), "smi-irq", 0, smi_irq);
        pcms->smbus = I2C_BUS(qdev_get_child_bus(DEVICE(piix4_pm), "i2c"));
        /* TODO: Populate SPD eeprom data.  */
        smbus_eeprom_init(pcms->smbus, 8, NULL, 0);

        object_property_add_link(OBJECT(machine), PC_MACHINE_ACPI_DEVICE_PROP,
                                 TYPE_HOTPLUG_HANDLER,
                                 (Object **)&x86ms->acpi_dev,
                                 object_property_allow_set_link,
                                 OBJ_PROP_LINK_STRONG);
        object_property_set_link(OBJECT(machine), PC_MACHINE_ACPI_DEVICE_PROP,
                                 piix4_pm, &error_abort);
    }

    if (machine->nvdimms_state->is_enabled) {
        nvdimm_init_acpi_state(machine->nvdimms_state, system_io,
                               x86_nvdimm_acpi_dsmio,
                               x86ms->fw_cfg, OBJECT(pcms));
    }
}

typedef enum PCSouthBridgeOption {
    PC_SOUTH_BRIDGE_OPTION_PIIX3,
    PC_SOUTH_BRIDGE_OPTION_PIIX4,
    PC_SOUTH_BRIDGE_OPTION_MAX,
} PCSouthBridgeOption;

static const QEnumLookup PCSouthBridgeOption_lookup = {
    .array = (const char *const[]) {
        [PC_SOUTH_BRIDGE_OPTION_PIIX3] = TYPE_PIIX3_DEVICE,
        [PC_SOUTH_BRIDGE_OPTION_PIIX4] = TYPE_PIIX4_PCI_DEVICE,
    },
    .size = PC_SOUTH_BRIDGE_OPTION_MAX
};

static int pc_get_south_bridge(Object *obj, Error **errp)
{
    PCMachineState *pcms = PC_MACHINE(obj);
    int i;

    for (i = 0; i < PCSouthBridgeOption_lookup.size; i++) {
        if (g_strcmp0(PCSouthBridgeOption_lookup.array[i],
                      pcms->south_bridge) == 0) {
            return i;
        }
    }

    error_setg(errp, "Invalid south bridge value set");
    return 0;
}

static void pc_set_south_bridge(Object *obj, int value, Error **errp)
{
    PCMachineState *pcms = PC_MACHINE(obj);

    if (value < 0) {
        error_setg(errp, "Value can't be negative");
        return;
    }

    if (value >= PCSouthBridgeOption_lookup.size) {
        error_setg(errp, "Value too big");
        return;
    }

    pcms->south_bridge = PCSouthBridgeOption_lookup.array[value];
}

#ifdef CONFIG_XEN
static void pc_xen_hvm_init(MachineState *machine)
{
    PCMachineState *pcms = PC_MACHINE(machine);

    if (!xen_enabled()) {
        error_report("xenfv machine requires the xen accelerator");
        exit(1);
    }

    pc_init1(machine, xen_igd_gfx_pt_enabled()
                      ? TYPE_IGD_PASSTHROUGH_I440FX_PCI_DEVICE
                      : TYPE_I440FX_PCI_DEVICE);

    xen_igd_reserve_slot(pcms->pcibus);
    pci_create_simple(pcms->pcibus, -1, "xen-platform");
}
#endif

static void pc_i440fx_init(MachineState *machine)
{
    pc_init1(machine, TYPE_I440FX_PCI_DEVICE);
}

#define DEFINE_I440FX_MACHINE(major, minor) \
    DEFINE_PC_VER_MACHINE(pc_i440fx, "pc-i440fx", pc_i440fx_init, false, NULL, major, minor);

#define DEFINE_I440FX_MACHINE_AS_LATEST(major, minor) \
    DEFINE_PC_VER_MACHINE(pc_i440fx, "pc-i440fx", pc_i440fx_init, true, "pc", major, minor);

static void pc_i440fx_machine_options(MachineClass *m)
{
    PCMachineClass *pcmc = PC_MACHINE_CLASS(m);
    ObjectClass *oc = OBJECT_CLASS(m);
    pcmc->default_south_bridge = TYPE_PIIX3_DEVICE;
    pcmc->pci_root_uid = 0;
    pcmc->default_cpu_version = 1;

    m->family = "pc_piix";
    m->desc = "Standard PC (i440FX + PIIX, 1996)";
    m->default_machine_opts = "firmware=bios-256k.bin";
    m->default_display = "std";
    m->default_nic = "e1000";
    m->no_floppy = !module_object_class_by_name(TYPE_ISA_FDC);
    m->no_parallel = !module_object_class_by_name(TYPE_ISA_PARALLEL);
    machine_class_allow_dynamic_sysbus_dev(m, TYPE_RAMFB_DEVICE);
    machine_class_allow_dynamic_sysbus_dev(m, TYPE_VMBUS_BRIDGE);
    machine_class_allow_dynamic_sysbus_dev(m, TYPE_UEFI_VARS_X64);

    object_class_property_add_enum(oc, "x-south-bridge", "PCSouthBridgeOption",
                                   &PCSouthBridgeOption_lookup,
                                   pc_get_south_bridge,
                                   pc_set_south_bridge);
    object_class_property_set_description(oc, "x-south-bridge",
                                     "Use a different south bridge than PIIX3");
    compat_props_add(m->compat_props,
                     pc_piix_compat_defaults, pc_piix_compat_defaults_len);
}

static void pc_i440fx_machine_11_2_options(MachineClass *m)
{
    pc_i440fx_machine_options(m);
}

DEFINE_I440FX_MACHINE_AS_LATEST(11, 2);

static void pc_i440fx_machine_11_1_options(MachineClass *m)
{
    pc_i440fx_machine_11_2_options(m);
    compat_props_add(m->compat_props, hw_compat_11_1, hw_compat_11_1_len);
    compat_props_add(m->compat_props, pc_compat_11_1, pc_compat_11_1_len);
}

DEFINE_I440FX_MACHINE(11, 1);

static void pc_i440fx_machine_11_0_options(MachineClass *m)
{
    pc_i440fx_machine_11_1_options(m);
    compat_props_add(m->compat_props, hw_compat_11_0, hw_compat_11_0_len);
    compat_props_add(m->compat_props, pc_compat_11_0, pc_compat_11_0_len);
}

DEFINE_I440FX_MACHINE(11, 0);

static void pc_i440fx_machine_10_2_options(MachineClass *m)
{
    pc_i440fx_machine_11_0_options(m);
    compat_props_add(m->compat_props, hw_compat_10_2, hw_compat_10_2_len);
    compat_props_add(m->compat_props, pc_compat_10_2, pc_compat_10_2_len);
}

DEFINE_I440FX_MACHINE(10, 2);

static void pc_i440fx_machine_10_1_options(MachineClass *m)
{
    pc_i440fx_machine_10_2_options(m);
    m->smbios_memory_device_size = 2047 * TiB;
    compat_props_add(m->compat_props, hw_compat_10_1, hw_compat_10_1_len);
    compat_props_add(m->compat_props, pc_compat_10_1, pc_compat_10_1_len);
}

DEFINE_I440FX_MACHINE(10, 1);

static void pc_i440fx_machine_10_0_options(MachineClass *m)
{
    pc_i440fx_machine_10_1_options(m);
    compat_props_add(m->compat_props, hw_compat_10_0, hw_compat_10_0_len);
    compat_props_add(m->compat_props, pc_compat_10_0, pc_compat_10_0_len);
}

DEFINE_I440FX_MACHINE(10, 0);

static void pc_i440fx_machine_9_2_options(MachineClass *m)
{
    pc_i440fx_machine_10_0_options(m);
    compat_props_add(m->compat_props, hw_compat_9_2, hw_compat_9_2_len);
    compat_props_add(m->compat_props, pc_compat_9_2, pc_compat_9_2_len);
}

DEFINE_I440FX_MACHINE(9, 2);

static void pc_i440fx_machine_9_1_options(MachineClass *m)
{
    pc_i440fx_machine_9_2_options(m);
    compat_props_add(m->compat_props, hw_compat_9_1, hw_compat_9_1_len);
    compat_props_add(m->compat_props, pc_compat_9_1, pc_compat_9_1_len);
}

DEFINE_I440FX_MACHINE(9, 1);

static void pc_i440fx_machine_9_0_options(MachineClass *m)
{
    PCMachineClass *pcmc = PC_MACHINE_CLASS(m);

    pc_i440fx_machine_9_1_options(m);
    m->smbios_memory_device_size = 16 * GiB;

    compat_props_add(m->compat_props, hw_compat_9_0, hw_compat_9_0_len);
    compat_props_add(m->compat_props, pc_compat_9_0, pc_compat_9_0_len);
    pcmc->isa_bios_alias = false;
}

DEFINE_I440FX_MACHINE(9, 0);

static void pc_i440fx_machine_8_2_options(MachineClass *m)
{
    PCMachineClass *pcmc = PC_MACHINE_CLASS(m);

    pc_i440fx_machine_9_0_options(m);

    compat_props_add(m->compat_props, hw_compat_8_2, hw_compat_8_2_len);
    compat_props_add(m->compat_props, pc_compat_8_2, pc_compat_8_2_len);
    /* For pc-i44fx-8.2 and 8.1, use SMBIOS 3.X by default */
    pcmc->default_smbios_ep_type = SMBIOS_ENTRY_POINT_TYPE_64;
}

DEFINE_I440FX_MACHINE(8, 2);

static void pc_i440fx_machine_8_1_options(MachineClass *m)
{
    PCMachineClass *pcmc = PC_MACHINE_CLASS(m);

    pc_i440fx_machine_8_2_options(m);
    pcmc->broken_32bit_mem_addr_check = true;

    compat_props_add(m->compat_props, hw_compat_8_1, hw_compat_8_1_len);
    compat_props_add(m->compat_props, pc_compat_8_1, pc_compat_8_1_len);
}

DEFINE_I440FX_MACHINE(8, 1);

static void pc_i440fx_machine_8_0_options(MachineClass *m)
{
    PCMachineClass *pcmc = PC_MACHINE_CLASS(m);

    pc_i440fx_machine_8_1_options(m);
    compat_props_add(m->compat_props, hw_compat_8_0, hw_compat_8_0_len);
    compat_props_add(m->compat_props, pc_compat_8_0, pc_compat_8_0_len);

    /* For pc-i44fx-8.0 and older, use SMBIOS 2.8 by default */
    pcmc->default_smbios_ep_type = SMBIOS_ENTRY_POINT_TYPE_32;
}

DEFINE_I440FX_MACHINE(8, 0);

static void pc_i440fx_machine_7_2_options(MachineClass *m)
{
    pc_i440fx_machine_8_0_options(m);
    compat_props_add(m->compat_props, hw_compat_7_2, hw_compat_7_2_len);
    compat_props_add(m->compat_props, pc_compat_7_2, pc_compat_7_2_len);
}

DEFINE_I440FX_MACHINE(7, 2)

static void pc_i440fx_machine_7_1_options(MachineClass *m)
{
    pc_i440fx_machine_7_2_options(m);
    compat_props_add(m->compat_props, hw_compat_7_1, hw_compat_7_1_len);
    compat_props_add(m->compat_props, pc_compat_7_1, pc_compat_7_1_len);
}

DEFINE_I440FX_MACHINE(7, 1);

static void pc_i440fx_machine_7_0_options(MachineClass *m)
{
    PCMachineClass *pcmc = PC_MACHINE_CLASS(m);
    pc_i440fx_machine_7_1_options(m);
    pcmc->enforce_amd_1tb_hole = false;
    compat_props_add(m->compat_props, hw_compat_7_0, hw_compat_7_0_len);
    compat_props_add(m->compat_props, pc_compat_7_0, pc_compat_7_0_len);
}

DEFINE_I440FX_MACHINE(7, 0);

static void pc_i440fx_machine_6_2_options(MachineClass *m)
{
    pc_i440fx_machine_7_0_options(m);
    compat_props_add(m->compat_props, hw_compat_6_2, hw_compat_6_2_len);
    compat_props_add(m->compat_props, pc_compat_6_2, pc_compat_6_2_len);
}

DEFINE_I440FX_MACHINE(6, 2);

static void pc_i440fx_machine_6_1_options(MachineClass *m)
{
    pc_i440fx_machine_6_2_options(m);
    compat_props_add(m->compat_props, hw_compat_6_1, hw_compat_6_1_len);
    compat_props_add(m->compat_props, pc_compat_6_1, pc_compat_6_1_len);
    m->smp_props.prefer_sockets = true;
}

DEFINE_I440FX_MACHINE(6, 1);

static void pc_i440fx_machine_6_0_options(MachineClass *m)
{
    pc_i440fx_machine_6_1_options(m);
    compat_props_add(m->compat_props, hw_compat_6_0, hw_compat_6_0_len);
    compat_props_add(m->compat_props, pc_compat_6_0, pc_compat_6_0_len);
}

DEFINE_I440FX_MACHINE(6, 0);

static void pc_i440fx_machine_5_2_options(MachineClass *m)
{
    pc_i440fx_machine_6_0_options(m);
    compat_props_add(m->compat_props, hw_compat_5_2, hw_compat_5_2_len);
    compat_props_add(m->compat_props, pc_compat_5_2, pc_compat_5_2_len);
}

DEFINE_I440FX_MACHINE(5, 2);

static void pc_i440fx_machine_5_1_options(MachineClass *m)
{
    PCMachineClass *pcmc = PC_MACHINE_CLASS(m);

    pc_i440fx_machine_5_2_options(m);
    compat_props_add(m->compat_props, hw_compat_5_1, hw_compat_5_1_len);
    compat_props_add(m->compat_props, pc_compat_5_1, pc_compat_5_1_len);
    pcmc->kvmclock_create_always = false;
    pcmc->pci_root_uid = 1;
}

DEFINE_I440FX_MACHINE(5, 1);

static void pc_i440fx_machine_5_0_options(MachineClass *m)
{
    pc_i440fx_machine_5_1_options(m);
    m->numa_mem_supported = true;
    compat_props_add(m->compat_props, hw_compat_5_0, hw_compat_5_0_len);
    compat_props_add(m->compat_props, pc_compat_5_0, pc_compat_5_0_len);
    m->auto_enable_numa_with_memdev = false;
}

DEFINE_I440FX_MACHINE(5, 0);

static void pc_i440fx_machine_4_2_options(MachineClass *m)
{
    pc_i440fx_machine_5_0_options(m);
    compat_props_add(m->compat_props, hw_compat_4_2, hw_compat_4_2_len);
    compat_props_add(m->compat_props, pc_compat_4_2, pc_compat_4_2_len);
}

DEFINE_I440FX_MACHINE(4, 2);

static void pc_i440fx_machine_4_1_options(MachineClass *m)
{
    pc_i440fx_machine_4_2_options(m);
    compat_props_add(m->compat_props, hw_compat_4_1, hw_compat_4_1_len);
    compat_props_add(m->compat_props, pc_compat_4_1, pc_compat_4_1_len);
}

DEFINE_I440FX_MACHINE(4, 1);

#ifdef CONFIG_XEN
static void xenfv_machine_4_2_options(MachineClass *m)
{
    pc_i440fx_machine_4_2_options(m);
    m->desc = "Xen Fully-virtualized PC";
    m->alias = "xenfv";
    m->max_cpus = HVM_MAX_VCPUS;
    m->default_machine_opts = "accel=xen,suppress-vmdesc=on";
}

DEFINE_PC_MACHINE(xenfv_4_2, "xenfv-4.2", pc_xen_hvm_init,
                  xenfv_machine_4_2_options);
#endif

/*
 * NVIDIA G72M ("m63", GeForce Go 7300) PCI graphics device.
 *
 * The Apple TV SEC/PEI firmware does not carry a generic VGA driver: it
 * enumerates the PCI bus expecting its embedded NVIDIA GPU (10de:01d7) and
 * hands control to its own NVIDIA UGA driver, which carries a copy of the
 * board VBIOS.  The register/BAR layout below mirrors the real machine
 * (atv-devicetree `lspci`, GPU on bus 01 behind the 00:01.0 PCIe port):
 *
 *   0x10  BAR0  16 MiB   registers
 *   0x14  BAR1 256 MiB   64-bit prefetchable VRAM aperture
 *   0x1c  BAR3  16 MiB   64-bit instance/pramin window
 *   0x30  ROM  128 KiB   m63 VBIOS (ATV_GPU_ROM=<file>)
 *
 * Register accesses are modelled sparsely (last value written wins, a few
 * power-on defaults) and can be logged verbatim with ATV_GPU_LOG=<file>,
 * which is how the driver's init/mode-set sequence is reverse engineered.
 */
OBJECT_DECLARE_SIMPLE_TYPE(ATVNvidiaGPUState, ATV_NVIDIA_GPU)

#define ATV_NVIDIA_DEVICE_G72M 0x01d7   /* GeForce Go 7300 */
#define ATV_NVIDIA_CHIPSET     0x0460   /* NV46 / G72M, PMC_BOOT_0[20:27] */
#define ATV_NVIDIA_REGS_SIZE   0x01000000  /* 16 MiB  BAR0  */
#define ATV_NVIDIA_FB_SIZE     0x04000000  /* 64 MiB VRAM (BAR1 aperture) */
#define ATV_NVIDIA_INST_SIZE   0x01000000  /* 16 MiB  BAR3  */
#define ATV_NVIDIA_REG_SLOTS   8192
#define ATV_NVIDIA_LOG_MAX     400000

typedef struct ATVNvidiaReg {
    uint32_t addr;
    uint32_t val;
} ATVNvidiaReg;

struct ATVNvidiaGPUState {
    PCIDevice dev;
    MemoryRegion regs;
    MemoryRegion fb;
    MemoryRegion inst;
    ATVNvidiaReg reg[ATV_NVIDIA_REG_SLOTS];
    FILE *log;
    unsigned nlog;
    bool cfgdbg;
    QemuConsole *con;
    QEMUTimer *timer;
    int disp_mode;
    int geom_w, geom_h, geom_depth;
    int text_cols, text_rows;
};

static void atv_nvidia_log(ATVNvidiaGPUState *s, char op, hwaddr addr,
                           unsigned size, uint64_t val)
{
    if (!s->log || s->nlog >= ATV_NVIDIA_LOG_MAX) {
        return;
    }
    fprintf(s->log, "%c %06x %u %08x\n", op, (unsigned)addr, size,
            (unsigned)val);
    if (++s->nlog == ATV_NVIDIA_LOG_MAX) {
        fprintf(s->log, "# log limit reached\n");
        fflush(s->log);
    }
}

/* Sparse 16 MiB register file: addr == ~0 marks a free slot. */
static ATVNvidiaReg *atv_nvidia_slot(ATVNvidiaGPUState *s, uint32_t addr)
{
    unsigned i = (addr >> 2) % ATV_NVIDIA_REG_SLOTS;

    while (s->reg[i].addr != 0xFFFFFFFF) {
        if (s->reg[i].addr == addr) {
            return &s->reg[i];
        }
        if (++i == ATV_NVIDIA_REG_SLOTS) {
            i = 0;
        }
    }
    return &s->reg[i];
}

static uint32_t atv_nvidia_reg32(ATVNvidiaGPUState *s, uint32_t addr)
{
    ATVNvidiaReg *r = atv_nvidia_slot(s, addr);

    if (r->addr == 0xFFFFFFFF) {
        /* power-on values the driver keys off before it writes anything */
        switch (addr) {
        case 0x000000:                  /* PMC_BOOT_0 */
            return (ATV_NVIDIA_CHIPSET << 20) | 0xA1;
        case 0x001000:                  /* PBUS_DEVID */
            return 0x046001d7;
        default:
            return 0;
        }
    }
    return r->val;
}

static uint64_t atv_nvidia_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    ATVNvidiaGPUState *s = opaque;
    uint32_t waddr = addr & ~3u;
    uint32_t val = atv_nvidia_reg32(s, waddr) >> ((addr & 3) * 8);
    uint64_t ret;

    if (size < 4) {
        val &= (1u << (size * 8)) - 1;
    }
    ret = val;
    atv_nvidia_log(s, 'r', addr, size, ret);
    return ret;
}

static void atv_nvidia_regs_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    ATVNvidiaGPUState *s = opaque;
    uint32_t waddr = addr & ~3u;
    unsigned shift = (addr & 3) * 8;
    uint32_t mask = (size >= 4) ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1) << shift;
    ATVNvidiaReg *r = atv_nvidia_slot(s, waddr);
    uint32_t cur = (r->addr == 0xFFFFFFFF) ? atv_nvidia_reg32(s, waddr) : r->val;

    r->addr = waddr;
    r->val = (cur & ~mask) | ((uint32_t)val << shift & mask);
    atv_nvidia_log(s, 'w', addr, size, val);
}

static const MemoryRegionOps atv_nvidia_regs_ops = {
    .read = atv_nvidia_regs_read,
    .write = atv_nvidia_regs_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * Display output.
 *
 * On the real machine the panel stays dark until the OS initialises the
 * display: the firmware itself never produces a picture.  Under QEMU the disk's
 * Linux loader draws a splash and hands the kernel a framebuffer, but the
 * kernel's first console is the legacy text one (it writes 80x35 cells to
 * 0xB8000), and once a linear framebuffer exists the pixels land in the BAR1
 * aperture.  Both sources are rendered here, so the machine has a picture (and
 * `screendump` works even with -display none).
 *
 *   ATV_GPU_DISPLAY=vram   render BAR1 as a linear framebuffer instead of text
 *   ATV_GPU_GEOM=WxHxD     framebuffer geometry (default 1920x1080x32)
 *   ATV_GPU_TEXT=CxR       text-console geometry (default 80x35)
 */
#define ATV_DISP_TEXT           0
#define ATV_DISP_VRAM           1
#define ATV_DISPLAY_REFRESH_MS  100
#define ATV_DISPLAY_VGA_TEXT    0x000b8000

static const uint32_t atv_nvidia_cga[16] = {
    0x000000, 0x0000aa, 0x00aa00, 0x00aaaa,
    0xaa0000, 0xaa00aa, 0xaa5500, 0xaaaaaa,
    0x555555, 0x5555ff, 0x55ff55, 0x55ffff,
    0xff5555, 0xff55ff, 0xffff55, 0xffffff,
};

static void atv_nvidia_draw_text(ATVNvidiaGPUState *s, DisplaySurface *surf)
{
    uint8_t cells[160 * 64 * 2];
    int cols = MIN(s->text_cols, 160);
    int rows = MIN(s->text_rows, 64);
    int w = surface_width(surf), h = surface_height(surf);
    int stride = surface_stride(surf) / 4;
    uint32_t *dst = surface_data(surf);
    int row, col;

    address_space_read(&address_space_memory, ATV_DISPLAY_VGA_TEXT,
                       MEMTXATTRS_UNSPECIFIED, cells, cols * rows * 2);
    for (row = 0; row < rows; row++) {
        int y0 = row * h / rows;
        int y1 = MAX(y0 + 1, (row + 1) * h / rows);

        for (col = 0; col < cols; col++) {
            int x0 = col * w / cols;
            int x1 = MAX(x0 + 1, (col + 1) * w / cols);
            const uint8_t *cell = cells + (row * cols + col) * 2;
            const uint8_t *glyph = vgafont16 + cell[0] * FONT_HEIGHT;
            uint32_t fg = atv_nvidia_cga[cell[1] & 0x0f];
            uint32_t bg = atv_nvidia_cga[(cell[1] >> 4) & 0x07];
            int x, y;

            for (y = y0; y < y1; y++) {
                uint8_t bits = glyph[(y - y0) * FONT_HEIGHT / (y1 - y0)];
                uint32_t *d = dst + (size_t)y * stride + x0;

                for (x = x0; x < x1; x++) {
                    int gx = (x - x0) * FONT_WIDTH / (x1 - x0);

                    *d++ = (bits & (0x80 >> gx)) ? fg : bg;
                }
            }
        }
    }
}

static void atv_nvidia_draw_vram(ATVNvidiaGPUState *s, DisplaySurface *surf)
{
    const uint8_t *src;
    uint8_t *dst = surface_data(surf);
    int stride = surface_stride(surf);
    int w = MIN(s->geom_w, surface_width(surf));
    int h = MIN(s->geom_h, surface_height(surf));
    int pitch = s->geom_w * 4;
    int y;

    if (s->geom_depth != 32) {
        return;
    }
    src = memory_region_get_ram_ptr(&s->fb);
    for (y = 0; y < h; y++) {
        memcpy(dst + (size_t)y * stride, src + (size_t)y * pitch, w * 4);
    }
}

static void atv_nvidia_render(ATVNvidiaGPUState *s)
{
    DisplaySurface *surf;

    if (!s->con) {
        return;
    }
    surf = qemu_console_surface(s->con);
    if (!surf || surface_is_placeholder(surf)) {
        return;
    }
    if (s->disp_mode == ATV_DISP_VRAM) {
        atv_nvidia_draw_vram(s, surf);
    } else {
        atv_nvidia_draw_text(s, surf);
    }
}

static void atv_nvidia_display_tick(void *opaque)
{
    ATVNvidiaGPUState *s = opaque;

    atv_nvidia_render(s);
    qemu_console_update_full(s->con);
    timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                        ATV_DISPLAY_REFRESH_MS);
}

static bool atv_nvidia_gfx_update(void *opaque)
{
    atv_nvidia_render(opaque);
    return true;
}

static const GraphicHwOps atv_nvidia_hw_ops = {
    .gfx_update = atv_nvidia_gfx_update,
};

static void atv_nvidia_display_init(ATVNvidiaGPUState *s)
{
    const char *mode = getenv("ATV_GPU_DISPLAY");
    const char *geom = getenv("ATV_GPU_GEOM");
    const char *text = getenv("ATV_GPU_TEXT");
    /* the ATV1's maximum output is 1080p60 */
    int w = 1920, h = 1080, d = 32;

    if (geom) {
        sscanf(geom, "%dx%dx%d", &w, &h, &d);
    }
    s->text_cols = 80;
    s->text_rows = 35;
    if (text) {
        sscanf(text, "%dx%d", &s->text_cols, &s->text_rows);
    }
    s->disp_mode = (mode && !strcmp(mode, "vram")) ? ATV_DISP_VRAM
                                                   : ATV_DISP_TEXT;
    s->geom_w = w;
    s->geom_h = h;
    s->geom_depth = d;

    s->con = qemu_graphic_console_create(DEVICE(s), 0, &atv_nvidia_hw_ops, s);
    qemu_console_resize(s->con, w, h);
    s->timer = timer_new_ms(QEMU_CLOCK_REALTIME, atv_nvidia_display_tick, s);
    timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 100);
}

static void atv_nvidia_gpu_realize(PCIDevice *pci_dev, Error **errp)
{
    ATVNvidiaGPUState *s = ATV_NVIDIA_GPU(pci_dev);
    uint8_t *c = pci_dev->config;
    const char *logpath = getenv("ATV_GPU_LOG");
    const char *rom = getenv("ATV_GPU_ROM");
    unsigned i;

    c[PCI_INTERRUPT_PIN] = 0x01;
    c[PCI_LATENCY_TIMER] = 0x40;

    for (i = 0; i < ATV_NVIDIA_REG_SLOTS; i++) {
        s->reg[i].addr = 0xFFFFFFFF;
    }
    if (logpath && *logpath) {
        s->log = fopen(logpath, "w");
        if (s->log) {
            setvbuf(s->log, NULL, _IOLBF, 0);
        }
    }
    s->cfgdbg = getenv("ATV_GPU_DEBUG") != NULL;

    memory_region_init_io(&s->regs, OBJECT(s), &atv_nvidia_regs_ops, s,
                          "atv-nvidia-regs", ATV_NVIDIA_REGS_SIZE);
    pci_register_bar(&s->dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->regs);

    memory_region_init_ram(&s->fb, OBJECT(s), "atv-nvidia-vram",
                           ATV_NVIDIA_FB_SIZE, errp);
    if (*errp) {
        return;
    }
    memory_region_set_log(&s->fb, true, DIRTY_MEMORY_VGA);
    pci_register_bar(&s->dev, 1, PCI_BASE_ADDRESS_MEM_PREFETCH |
                                  PCI_BASE_ADDRESS_MEM_TYPE_64, &s->fb);

    memory_region_init_ram(&s->inst, OBJECT(s), "atv-nvidia-inst",
                           ATV_NVIDIA_INST_SIZE, errp);
    if (*errp) {
        return;
    }
    pci_register_bar(&s->dev, 3, PCI_BASE_ADDRESS_MEM_TYPE_64, &s->inst);

    /* pci_qdev_realize() loads the ROM bar contents after this hook runs */
    if (rom && *rom) {
        pci_dev->romfile = g_strdup(rom);
    }

    atv_nvidia_display_init(s);

    if (getenv("ATV_GPU_DEBUG")) {
        error_report("atv-nvidia-gpu realize end: BAR0=%08x BAR1=%08x BAR3=%08x",
                     pci_get_long(c + PCI_BASE_ADDRESS_0),
                     pci_get_long(c + PCI_BASE_ADDRESS_1),
                     pci_get_long(c + PCI_BASE_ADDRESS_3));
    }
}

/*
 * The ATV firmware never allocates PCI resources: the root bridge reports an
 * empty resource list and the PciHostBridgeResourceAllocation protocol is
 * absent, so the bus driver sizes each BAR (write ~0, read back the mask) and
 * then restores what it read - which is 0 unless the device came up with the
 * BARs already programmed.  Seed them here, in reset, with the values a real
 * ATV1 boot leaves in the GPU (atv-devicetree lspci):
 *
 *   0x21000000  16 MiB   registers
 *   0x10000000  256 MiB  framebuffer aperture (64-bit prefetchable)
 *   0x20000000  16 MiB   instance window (64-bit)
 *
 * The restore-write then re-creates the mappings, which is why this has to go
 * through pci_default_write_config() and not a raw poke of the config space.
 */
#define ATV_NVIDIA_REG_BASE  0x21000000
#define ATV_NVIDIA_FB_BASE   0x10000000
#define ATV_NVIDIA_INST_BASE 0x20000000

static const struct {
    uint8_t off;
    uint32_t value;     /* type bits included, as PCI config wants them */
} atv_nvidia_bars[] = {
    { PCI_BASE_ADDRESS_0, ATV_NVIDIA_REG_BASE },
    { PCI_BASE_ADDRESS_1, ATV_NVIDIA_FB_BASE | PCI_BASE_ADDRESS_MEM_PREFETCH |
                          PCI_BASE_ADDRESS_MEM_TYPE_64 },
    { PCI_BASE_ADDRESS_2, 0 },                      /* BAR1 upper half */
    { PCI_BASE_ADDRESS_3, ATV_NVIDIA_INST_BASE | PCI_BASE_ADDRESS_MEM_TYPE_64 },
    { PCI_BASE_ADDRESS_4, 0 },                      /* BAR3 upper half */
};

static void atv_nvidia_bars_program(PCIDevice *d)
{
    unsigned i;

    /*
     * The addresses alone are not enough: pci_bar_address() reports every
     * memory BAR unmapped while PCI_COMMAND has decode off
     * (hw/pci/pci.c: `if (!(cmd & PCI_COMMAND_MEMORY)) return PCI_BAR_UNMAPPED`).
     * On the real machine the platform store hands the firmware a PCI resource
     * list, so its bus driver both assigns the windows *and* enables decode.
     * Here the root bridge reports an empty resource list, so nothing ever sets
     * the bits and the GPU's MMIO never becomes visible to the firmware - which
     * is why the m63 driver ran its whole Start() path without a single
     * register access.  Seed the decode bits together with the addresses.
     */
    pci_default_write_config(d, PCI_COMMAND,
                             PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
                             PCI_COMMAND_MASTER, 2);
    for (i = 0; i < ARRAY_SIZE(atv_nvidia_bars); i++) {
        pci_default_write_config(d, atv_nvidia_bars[i].off,
                                 atv_nvidia_bars[i].value, 4);
    }
}

static void atv_nvidia_gpu_reset(DeviceState *dev)
{
    atv_nvidia_bars_program(PCI_DEVICE(dev));
}

/*
 * The bus driver sizes every BAR (write ~0, read back the mask) and then
 * writes back whatever it saved.  Because it has no resources to hand out,
 * that saved value is 0 and every device ends up with no MMIO window at all.
 * Let the sizing probe through (PciBus needs the size) but restore the real
 * ATV addresses for any other BAR write, so the windows stay mapped the way
 * they are on the real machine.
 */
static void atv_nvidia_config_write(PCIDevice *d, uint32_t addr, uint32_t val,
                                    int len)
{
    ATVNvidiaGPUState *s = ATV_NVIDIA_GPU(d);
    bool probe = (len == 4 && val == 0xFFFFFFFF);
    bool hit = false;
    unsigned i;

    if (s->cfgdbg && (ranges_overlap(addr, len, PCI_COMMAND, 2) ||
                      ranges_overlap(addr, len, PCI_BASE_ADDRESS_0, 0x18))) {
        error_report("atv-gpu cfg w off=%02x val=%0*x len=%d cmd=%04x",
                     addr, len * 2, val, len,
                     pci_get_word(d->config + PCI_COMMAND));
    }
    pci_default_write_config(d, addr, val, len);

    for (i = 0; i < ARRAY_SIZE(atv_nvidia_bars); i++) {
        if (ranges_overlap(addr, len, atv_nvidia_bars[i].off, 4)) {
            hit = true;
        }
    }
    if (hit && !probe) {
        atv_nvidia_bars_program(d);
    }
}

static void atv_nvidia_gpu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = atv_nvidia_gpu_realize;
    k->config_write = atv_nvidia_config_write;
    k->vendor_id = PCI_VENDOR_ID_NVIDIA;
    k->device_id = ATV_NVIDIA_DEVICE_G72M;
    k->subsystem_vendor_id = PCI_VENDOR_ID_NVIDIA;
    k->subsystem_id = ATV_NVIDIA_DEVICE_G72M;
    k->revision = 0xa1;
    k->class_id = PCI_CLASS_DISPLAY_VGA;
    device_class_set_legacy_reset(dc, atv_nvidia_gpu_reset);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->desc = "Apple TV NVIDIA G72M (GeForce Go 7300) shim";
}

static const TypeInfo atv_nvidia_gpu_info = {
    .name          = TYPE_ATV_NVIDIA_GPU,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(ATVNvidiaGPUState),
    .class_init    = atv_nvidia_gpu_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void atv_nvidia_gpu_register_types(void)
{
    type_register_static(&atv_nvidia_gpu_info);
}

type_init(atv_nvidia_gpu_register_types)
