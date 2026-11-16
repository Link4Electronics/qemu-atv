/*
 * QEMU i440FX PCI Bridge Emulation
 *
 * Copyright (c) 2006 Fabrice Bellard
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
#include "hw/pci/pcie_host.h"
#include "hw/pci-host/i440fx.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "qom/object.h"

/*
 * I440FX chipset data sheet.
 * https://wiki.qemu.org/File:29054901.pdf
 */

OBJECT_DECLARE_SIMPLE_TYPE(I440FXState, I440FX_PCI_HOST_BRIDGE)

struct I440FXState {
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

#define I440FX_PAM      0x59
#define I440FX_PAM_SIZE 7
#define I440FX_SMRAM    0x72

/* Apple TV (945GM MCH) MMCONFIG registers in the host bridge. */
#define ATV_PCIEXBAR    0x48
#define ATV_TOP_OF_MEM  0x9c

/* Keep it 2G to comply with older win32 guests */
#define I440FX_PCI_HOST_HOLE64_SIZE_DEFAULT (1ULL << 31)

/* Older coreboot versions (4.0 and older) read a config register that doesn't
 * exist in real hardware, to get the RAM size from QEMU.
 */
#define I440FX_COREBOOT_RAM_SIZE 0x57

/*
 * Apple TV (945GM MCH) MMCONFIG / ECAM window.
 *
 * PCIEXBAR (00:00.0 offset 0x48) decodes as: bit 0 = enable, bits 2:1 =
 * window size (0b00 256 MB, 0b01 128 MB, 0b10 64 MB, 0b11 32 MB), bits
 * 31:size_shift = base.  The ATV firmware programs 0xf0000005 (64 MB at
 * 0xf0000000) and from then on performs EVERY PCI config access as a
 * memory access at base + (bus << 20) + (dev << 15) + (fn << 12) + reg
 * instead of going through CF8/CFC.  Without this window the accesses
 * land on the erased atv-nvram stub, every device reads back 0xff and
 * the bus scan discovers nothing.
 */
static PCIDevice *atv_mmcfg_dev(PCII440FXState *s, hwaddr addr)
{
    return pci_find_device(pci_get_bus(&s->parent_obj),
                           PCIE_MMCFG_BUS(addr), PCIE_MMCFG_DEVFN(addr));
}

static uint64_t atv_mmcfg_read(void *opaque, hwaddr addr, unsigned len)
{
    PCII440FXState *s = opaque;
    PCIDevice *dev = atv_mmcfg_dev(s, addr);
    uint32_t off = PCIE_MMCFG_CONFOFFSET(addr);
    uint64_t v;

    if (dev == NULL) {
        v = ~0x0;
    } else {
        v = pci_host_config_read_common(dev, off, pci_config_size(dev), len);
    }
    if (getenv("ATV_DEBUG_MMCFG") != NULL) {
        fprintf(stderr, "ATV MMCFG RD %03x:%02x.%x %03x len=%u -> 0x%llx\n",
                (unsigned)PCIE_MMCFG_BUS(addr),
                (unsigned)(PCIE_MMCFG_DEVFN(addr) >> 3),
                (unsigned)(PCIE_MMCFG_DEVFN(addr) & 7), (unsigned)off, len,
                (unsigned long long)v);
    }
    return v;
}

static void atv_mmcfg_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned len)
{
    PCII440FXState *s = opaque;
    PCIDevice *dev = atv_mmcfg_dev(s, addr);
    uint32_t off = PCIE_MMCFG_CONFOFFSET(addr);

    if (getenv("ATV_DEBUG_MMCFG") != NULL) {
        fprintf(stderr, "ATV MMCFG WR %03x:%02x.%x %03x len=%u = 0x%llx\n",
                (unsigned)PCIE_MMCFG_BUS(addr),
                (unsigned)(PCIE_MMCFG_DEVFN(addr) >> 3),
                (unsigned)(PCIE_MMCFG_DEVFN(addr) & 7), (unsigned)off, len,
                (unsigned long long)val);
    }
    if (dev == NULL) {
        return;
    }
    pci_host_config_write_common(dev, off, pci_config_size(dev), val, len);
}

static const MemoryRegionOps atv_mmcfg_ops = {
    .read = atv_mmcfg_read,
    .write = atv_mmcfg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void atv_mmcfg_update(PCII440FXState *s)
{
    uint32_t bar = pci_default_read_config(&s->parent_obj, ATV_PCIEXBAR, 4);
    hwaddr size = 256 * MiB;
    bool enable = bar & 1;

    if (getenv("ATV_MMCFG_OFF") != NULL) {
        enable = false;
    }

    switch ((bar >> 1) & 3) {
    case 0:
        size = 256 * MiB;
        break;
    case 1:
        size = 128 * MiB;
        break;
    case 2:
        size = 64 * MiB;
        break;
    case 3:
        size = 32 * MiB;
        break;
    }

    memory_region_transaction_begin();
    if (s->atv_mmcfg_mapped) {
        memory_region_del_subregion(get_system_memory(), &s->atv_mmcfg);
        s->atv_mmcfg_mapped = false;
    }
    if (s->atv_mmcfg_size != size) {
        memory_region_set_size(&s->atv_mmcfg, size);
        s->atv_mmcfg_size = size;
    }
    if (enable) {
        s->atv_mmcfg_base = bar & ~(size - 1);
        memory_region_add_subregion_overlap(get_system_memory(),
                                            s->atv_mmcfg_base,
                                            &s->atv_mmcfg, 2);
        s->atv_mmcfg_mapped = true;
    }
    memory_region_transaction_commit();

    if (getenv("ATV_DEBUG_MMCFG") != NULL) {
        fprintf(stderr, "ATV MMCONFIG %s base=0x%08" HWADDR_PRIx
                " size=0x%" HWADDR_PRIx "\n",
                enable ? "on" : "off", s->atv_mmcfg_base, size);
    }
}

static void i440fx_realize(PCIDevice *dev, Error **errp)
{
    dev->config[I440FX_SMRAM] = 0x02;

    if (object_property_get_bool(qdev_get_machine(), "iommu", NULL)) {
        warn_report("i440fx doesn't support emulated iommu");
    }

    if (object_property_get_uint(qdev_get_machine(), "car-size", NULL) > 0) {
        PCII440FXState *f = I440FX_PCI_DEVICE(dev);
        const char *hb = getenv("ATV_HB_ID");
        unsigned long hb_id = hb ? strtoul(hb, NULL, 0) : 0x27a0;
        const char *td = getenv("ATV_TOPDRAM");
        long top_dram = td ? strtol(td, NULL, 0) : 0x10;

        /* Apple TV (945GM MCH) atv mode: reveal the real host-bridge
         * config register 0xE4 (0xA10A2503, bit 17 set) that the SEC
         * firmware uses as the "no TPM / skip TPM probe" gate. Without it
         * the SEC spins forever polling the (absent) TPM at 0xfed40000. */
        pci_set_long(dev->config + 0xE4, 0xa10a2503);

        /* The SEC sizes DRAM from the byte at 00:00.0 offset 0x9c (the
         * "top of memory" value, shifted left 24).  Real ATV1 hardware
         * reads 0x10 there for 256 MB and the firmware itself writes the
         * same byte back later; seed it so the read works before that.
         * This is the same byte the legacy atv-nvram stub used to fake. */
        dev->config[ATV_TOP_OF_MEM] = (uint8_t)top_dram;

        /* The firmware reaches all PCI config space through MMCONFIG,
         * so the window has to exist before the first PCIEXBAR write. */
        memory_region_init_io(&f->atv_mmcfg, OBJECT(dev), &atv_mmcfg_ops, f,
                              "atv-mmcfg", 256 * MiB);
        f->atv_mmcfg_size = 256 * MiB;
        atv_mmcfg_update(f);

        /* The firmware's PEIM matches the 945GM MCH (8086:27a0) and only
         * then builds the PCI root-bridge HOB.  With a plain i440FX ID
         * (8086:1237) PciHostBridgeDxe registers EFI_PCI_ROOT_BRIDGE_IO
         * but installs no instance, so the bus is never enumerated and no
         * storage driver can ever bind.  ATV_HB_ID=0 restores the stock ID
         * for A/B comparison. */
        if (hb_id != 0) {
            pci_config_set_vendor_id(dev->config, PCI_VENDOR_ID_INTEL);
            pci_config_set_device_id(dev->config, (uint16_t)hb_id);
            dev->config[PCI_REVISION_ID] = 0x03;
        }
    }
}

static void i440fx_update_memory_mappings(PCII440FXState *d)
{
    int i;
    PCIDevice *pd = PCI_DEVICE(d);

    memory_region_transaction_begin();
    for (i = 0; i < ARRAY_SIZE(d->pam_regions); i++) {
        pam_update(&d->pam_regions[i], i,
                   pd->config[I440FX_PAM + DIV_ROUND_UP(i, 2)]);
    }
    memory_region_set_enabled(&d->smram_region,
                              !(pd->config[I440FX_SMRAM] & SMRAM_D_OPEN));
    memory_region_set_enabled(&d->smram,
                              pd->config[I440FX_SMRAM] & SMRAM_G_SMRAME);
    memory_region_transaction_commit();
}


static void i440fx_write_config(PCIDevice *dev,
                                uint32_t address, uint32_t val, int len)
{
    PCII440FXState *d = I440FX_PCI_DEVICE(dev);

    /* XXX: implement SMRAM.D_LOCK */
    pci_default_write_config(dev, address, val, len);
    if (object_property_get_uint(qdev_get_machine(), "car-size", NULL) > 0) {
        fprintf(stderr, "ATV MCH cfg WR off=0x%02x len=%d = 0x%08x\n",
                address, len, val);
        if (ranges_overlap(address, len, ATV_PCIEXBAR, 4)) {
            atv_mmcfg_update(d);
        }
    }
    if (ranges_overlap(address, len, I440FX_PAM, I440FX_PAM_SIZE) ||
        range_covers_byte(address, len, I440FX_SMRAM)) {
        i440fx_update_memory_mappings(d);
    }
}

static uint32_t i440fx_read_config(PCIDevice *dev, uint32_t address, int len)
{
    uint32_t v = pci_default_read_config(dev, address, len);

    /* ATV debug: log MCH (dev0) config reads the SEC relies on. */
    if (object_property_get_uint(qdev_get_machine(), "car-size", NULL) > 0) {
        if (address <= 0x1ff) {
            fprintf(stderr, "ATV MCH cfg read off=0x%02x len=%d -> 0x%08x\n",
                    address, len, v);
        }
    }
    return v;
}

static int i440fx_post_load(void *opaque, int version_id)
{
    PCII440FXState *d = opaque;

    i440fx_update_memory_mappings(d);
    return 0;
}

/*
 * Apple TV (car-size > 0) platform reset.
 *
 * The SEC reset entry programs PCIEXBAR=0xe0000001 and then reads
 * 00:00.0 offset 0xdc; a non-zero value makes it issue another CF9
 * global reset, so a stale register loops the machine forever.  On real
 * hardware both PCIEXBAR and 0xdc come back as zero from PLTRST, so
 * model that here (QEMU's generic PCI reset only clears the standard
 * header words).
 */
static void i440fx_reset(DeviceState *dev)
{
    PCII440FXState *f = I440FX_PCI_DEVICE(dev);

    if (object_property_get_uint(qdev_get_machine(), "car-size", NULL) > 0) {
        pci_set_long(f->parent_obj.config + ATV_PCIEXBAR, 0);
        pci_set_long(f->parent_obj.config + 0xdc, 0);
        atv_mmcfg_update(f);
    }
}

static const VMStateDescription vmstate_i440fx = {
    .name = "I440FX",
    .version_id = 3,
    .minimum_version_id = 3,
    .post_load = i440fx_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, PCII440FXState),
        /* Used to be smm_enabled, which was basically always zero because
         * SeaBIOS hardly uses SMM.  SMRAM is now handled by CPU code.
         */
        VMSTATE_UNUSED(1),
        VMSTATE_END_OF_LIST()
    }
};

static void i440fx_pcihost_get_pci_hole_start(Object *obj, Visitor *v,
                                              const char *name, void *opaque,
                                              Error **errp)
{
    I440FXState *s = I440FX_PCI_HOST_BRIDGE(obj);
    uint64_t val64;
    uint32_t value;

    val64 = range_is_empty(&s->pci_hole) ? 0 : range_lob(&s->pci_hole);
    value = val64;
    assert(value == val64);
    visit_type_uint32(v, name, &value, errp);
}

static void i440fx_pcihost_get_pci_hole_end(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    I440FXState *s = I440FX_PCI_HOST_BRIDGE(obj);
    uint64_t val64;
    uint32_t value;

    val64 = range_is_empty(&s->pci_hole) ? 0 : range_upb(&s->pci_hole) + 1;
    value = val64;
    assert(value == val64);
    visit_type_uint32(v, name, &value, errp);
}

/*
 * The 64bit PCI hole start is set by the Guest firmware
 * as the address of the first 64bit PCI MEM resource.
 * If no PCI device has resources on the 64bit area,
 * the 64bit PCI hole will start after "over 4G RAM" and the
 * reserved space for memory hotplug if any.
 */
static uint64_t i440fx_pcihost_get_pci_hole64_start_value(Object *obj)
{
    PCIHostState *h = PCI_HOST_BRIDGE(obj);
    I440FXState *s = I440FX_PCI_HOST_BRIDGE(obj);
    Range w64;
    uint64_t value;

    pci_bus_get_w64_range(h->bus, &w64);
    value = range_is_empty(&w64) ? 0 : range_lob(&w64);
    if (!value && s->pci_hole64_fix) {
        value = pc_pci_hole64_start();
    }
    return value;
}

static void i440fx_pcihost_get_pci_hole64_start(Object *obj, Visitor *v,
                                                const char *name,
                                                void *opaque, Error **errp)
{
    PCIHostState *h = PCI_HOST_BRIDGE(obj);
    uint64_t hole64_start;

    if (!h->bus) {
        error_setg(errp, "PCI host bridge not realized");
        return;
    }
    hole64_start = i440fx_pcihost_get_pci_hole64_start_value(obj);
    visit_type_uint64(v, name, &hole64_start, errp);
}

/*
 * The 64bit PCI hole end is set by the Guest firmware
 * as the address of the last 64bit PCI MEM resource.
 * Then it is expanded to the PCI_HOST_PROP_PCI_HOLE64_SIZE
 * that can be configured by the user.
 */
static void i440fx_pcihost_get_pci_hole64_end(Object *obj, Visitor *v,
                                              const char *name, void *opaque,
                                              Error **errp)
{
    PCIHostState *h = PCI_HOST_BRIDGE(obj);
    I440FXState *s = I440FX_PCI_HOST_BRIDGE(obj);
    uint64_t hole64_start;
    Range w64;
    uint64_t value, hole64_end;

    if (!h->bus) {
        error_setg(errp, "PCI host bridge not realized");
        return;
    }
    hole64_start = i440fx_pcihost_get_pci_hole64_start_value(obj);
    pci_bus_get_w64_range(h->bus, &w64);
    value = range_is_empty(&w64) ? 0 : range_upb(&w64) + 1;
    hole64_end = ROUND_UP(hole64_start + s->pci_hole64_size, 1ULL << 30);
    if (s->pci_hole64_fix && value < hole64_end) {
        value = hole64_end;
    }
    visit_type_uint64(v, name, &value, errp);
}

static void i440fx_pcihost_initfn(Object *obj)
{
    I440FXState *s = I440FX_PCI_HOST_BRIDGE(obj);
    PCIHostState *phb = PCI_HOST_BRIDGE(obj);

    memory_region_init_io(&phb->conf_mem, obj, &pci_host_conf_le_ops, phb,
                          "pci-conf-idx", 4);
    memory_region_init_io(&phb->data_mem, obj, &pci_host_data_le_ops, phb,
                          "pci-conf-data", 4);

    object_property_add_link(obj, PCI_HOST_PROP_RAM_MEM, TYPE_MEMORY_REGION,
                             (Object **) &s->ram_memory,
                             qdev_prop_allow_set_link_before_realize, 0);

    object_property_add_link(obj, PCI_HOST_PROP_PCI_MEM, TYPE_MEMORY_REGION,
                             (Object **) &s->pci_address_space,
                             qdev_prop_allow_set_link_before_realize, 0);

    object_property_add_link(obj, PCI_HOST_PROP_SYSTEM_MEM, TYPE_MEMORY_REGION,
                             (Object **) &s->system_memory,
                             qdev_prop_allow_set_link_before_realize, 0);

    object_property_add_link(obj, PCI_HOST_PROP_IO_MEM, TYPE_MEMORY_REGION,
                             (Object **) &s->io_memory,
                             qdev_prop_allow_set_link_before_realize, 0);
}

static void i440fx_pcihost_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    I440FXState *s = I440FX_PCI_HOST_BRIDGE(dev);
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    PCIBus *b;
    PCIDevice *d;
    PCII440FXState *f;
    unsigned i;

    memory_region_add_subregion(s->io_memory, 0xcf8, &phb->conf_mem);
    sysbus_init_ioports(sbd, 0xcf8, 4);

    memory_region_add_subregion(s->io_memory, 0xcfc, &phb->data_mem);
    sysbus_init_ioports(sbd, 0xcfc, 4);

    /* register i440fx 0xcf8 port as coalesced pio */
    memory_region_set_flush_coalesced(&phb->data_mem);
    memory_region_add_coalescing(&phb->conf_mem, 0, 4);

    b = pci_root_bus_new(dev, NULL, s->pci_address_space,
                         s->io_memory, 0, TYPE_PCI_BUS);
    phb->bus = b;

    d = pci_create_simple(b, 0, s->pci_type);
    f = I440FX_PCI_DEVICE(d);

    range_set_bounds(&s->pci_hole, s->below_4g_mem_size,
                     IO_APIC_DEFAULT_ADDRESS - 1);

    /* setup pci memory mapping */
    pc_pci_as_mapping_init(s->system_memory, s->pci_address_space);

    /* if *disabled* show SMRAM to all CPUs */
    memory_region_init_alias(&f->smram_region, OBJECT(d), "smram-region",
                             s->pci_address_space, SMRAM_C_BASE, SMRAM_C_SIZE);
    memory_region_add_subregion_overlap(s->system_memory, SMRAM_C_BASE,
                                        &f->smram_region, 1);
    memory_region_set_enabled(&f->smram_region, true);

    /* smram, as seen by SMM CPUs */
    memory_region_init(&f->smram, OBJECT(d), "smram", 4 * GiB);
    memory_region_set_enabled(&f->smram, true);
    memory_region_init_alias(&f->low_smram, OBJECT(d), "smram-low",
                             s->ram_memory, SMRAM_C_BASE, SMRAM_C_SIZE);
    memory_region_set_enabled(&f->low_smram, true);
    memory_region_add_subregion(&f->smram, SMRAM_C_BASE, &f->low_smram);
    object_property_add_const_link(qdev_get_machine(), "smram",
                                   OBJECT(&f->smram));

    init_pam(&f->pam_regions[0], OBJECT(d), s->ram_memory, s->system_memory,
             s->pci_address_space, PAM_BIOS_BASE, PAM_BIOS_SIZE);
    for (i = 0; i < ARRAY_SIZE(f->pam_regions) - 1; ++i) {
        init_pam(&f->pam_regions[i + 1], OBJECT(d), s->ram_memory,
                 s->system_memory, s->pci_address_space,
                 PAM_EXPAN_BASE + i * PAM_EXPAN_SIZE, PAM_EXPAN_SIZE);
    }

    ram_addr_t ram_size = s->below_4g_mem_size + s->above_4g_mem_size;
    ram_size = ram_size / 8 / 1024 / 1024;
    if (ram_size > 255) {
        ram_size = 255;
    }
    d->config[I440FX_COREBOOT_RAM_SIZE] = ram_size;

    i440fx_update_memory_mappings(f);
}

static void i440fx_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = i440fx_realize;
    k->config_write = i440fx_write_config;
    k->config_read = i440fx_read_config;
    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = PCI_DEVICE_ID_INTEL_82441;
    k->revision = 0x02;
    k->class_id = PCI_CLASS_BRIDGE_HOST;
    dc->desc = "Host bridge";
    dc->vmsd = &vmstate_i440fx;
    device_class_set_legacy_reset(dc, i440fx_reset);
    /*
     * PCI-facing part of the host bridge, not usable without the
     * host-facing part, which can't be device_add'ed, yet.
     */
    dc->user_creatable = false;
    dc->hotpluggable   = false;
}

static const TypeInfo i440fx_info = {
    .name          = TYPE_I440FX_PCI_DEVICE,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCII440FXState),
    .class_init    = i440fx_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static const char *i440fx_pcihost_root_bus_path(PCIHostState *host_bridge,
                                                PCIBus *rootbus)
{
    return "0000:00";
}

static const Property i440fx_props[] = {
    DEFINE_PROP_SIZE(PCI_HOST_PROP_PCI_HOLE64_SIZE, I440FXState,
                     pci_hole64_size, I440FX_PCI_HOST_HOLE64_SIZE_DEFAULT),
    DEFINE_PROP_SIZE(PCI_HOST_BELOW_4G_MEM_SIZE, I440FXState,
                     below_4g_mem_size, 0),
    DEFINE_PROP_SIZE(PCI_HOST_ABOVE_4G_MEM_SIZE, I440FXState,
                     above_4g_mem_size, 0),
    DEFINE_PROP_BOOL("x-pci-hole64-fix", I440FXState, pci_hole64_fix, true),
    DEFINE_PROP_STRING(I440FX_HOST_PROP_PCI_TYPE, I440FXState, pci_type),
};

static void i440fx_pcihost_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIHostBridgeClass *hc = PCI_HOST_BRIDGE_CLASS(klass);

    hc->root_bus_path = i440fx_pcihost_root_bus_path;
    dc->realize = i440fx_pcihost_realize;
    dc->fw_name = "pci";
    device_class_set_props(dc, i440fx_props);
    /* Reason: needs to be wired up by pc_init1 */
    dc->user_creatable = false;

    object_class_property_add(klass, PCI_HOST_PROP_PCI_HOLE_START, "uint32",
                              i440fx_pcihost_get_pci_hole_start,
                              NULL, NULL, NULL);

    object_class_property_add(klass, PCI_HOST_PROP_PCI_HOLE_END, "uint32",
                              i440fx_pcihost_get_pci_hole_end,
                              NULL, NULL, NULL);

    object_class_property_add(klass, PCI_HOST_PROP_PCI_HOLE64_START, "uint64",
                              i440fx_pcihost_get_pci_hole64_start,
                              NULL, NULL, NULL);

    object_class_property_add(klass, PCI_HOST_PROP_PCI_HOLE64_END, "uint64",
                              i440fx_pcihost_get_pci_hole64_end,
                              NULL, NULL, NULL);
}

static const TypeInfo i440fx_pcihost_info = {
    .name          = TYPE_I440FX_PCI_HOST_BRIDGE,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(I440FXState),
    .instance_init = i440fx_pcihost_initfn,
    .class_init    = i440fx_pcihost_class_init,
};

static void i440fx_register_types(void)
{
    type_register_static(&i440fx_info);
    type_register_static(&i440fx_pcihost_info);
}

type_init(i440fx_register_types)
