/*
 * QEMU PIIX PCI ISA Bridge Emulation
 *
 * Copyright (c) 2006 Fabrice Bellard
 * Copyright (c) 2018 Hervé Poussineau
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
#include "qemu/range.h"
#include "qapi/error.h"
#include "hw/dma/i8257.h"
#include "hw/southbridge/piix.h"
#include "hw/timer/i8254.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/ide/piix.h"
#include "hw/intc/i8259.h"
#include "hw/isa/isa.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "migration/vmstate.h"
#include "hw/acpi/acpi_aml_interface.h"

static void piix_set_irq_pic(PIIXState *s, int pic_irq)
{
    qemu_set_irq(s->isa_irqs_in[pic_irq],
                 !!(s->pic_levels &
                    (((1ULL << PIIX_NUM_PIRQS) - 1) <<
                     (pic_irq * PIIX_NUM_PIRQS))));
}

static void piix_set_pci_irq_level_internal(PIIXState *s, int pirq, int level)
{
    int pic_irq;
    uint64_t mask;

    pic_irq = s->dev.config[PIIX_PIRQCA + pirq];
    if (pic_irq >= ISA_NUM_IRQS) {
        return;
    }

    mask = 1ULL << ((pic_irq * PIIX_NUM_PIRQS) + pirq);
    s->pic_levels &= ~mask;
    s->pic_levels |= mask * !!level;
}

static void piix_set_pci_irq_level(PIIXState *s, int pirq, int level)
{
    int pic_irq;

    pic_irq = s->dev.config[PIIX_PIRQCA + pirq];
    if (pic_irq >= ISA_NUM_IRQS) {
        return;
    }

    piix_set_pci_irq_level_internal(s, pirq, level);

    piix_set_irq_pic(s, pic_irq);
}

static void piix_set_pci_irq(void *opaque, int pirq, int level)
{
    PIIXState *s = opaque;
    piix_set_pci_irq_level(s, pirq, level);
}

static void piix_request_i8259_irq(void *opaque, int irq, int level)
{
    PIIXState *s = opaque;
    qemu_set_irq(s->cpu_intr, level);
}

static PCIINTxRoute piix_route_intx_pin_to_irq(void *opaque, int pin)
{
    PCIDevice *pci_dev = opaque;
    int irq = pci_dev->config[PIIX_PIRQCA + pin];
    PCIINTxRoute route;

    if (irq < ISA_NUM_IRQS) {
        route.mode = PCI_INTX_ENABLED;
        route.irq = irq;
    } else {
        route.mode = PCI_INTX_DISABLED;
        route.irq = -1;
    }
    return route;
}

/*
 * ATV/ICH7 PIRQ router.
 *
 * The real board's LPC function has eight PIRQ route registers and wires each
 * PIRQ line to both interrupt controllers: the IO-APIC sees it on the fixed
 * GSI 16+n, the 8259 sees it on the ISA IRQ named in the register (bit 7 set
 * means "PIC path disabled").  QEMU's stock PIIX3 only implements four lines
 * and only the 8259 half, which is why an ATV guest that has not discovered an
 * IO-APIC (no MADT, no MP table - exactly the current bring-up state) gets no
 * PCI interrupts: 0x60..0x63 hold 0x80 after reset, i.e. everything is
 * "APIC only" and the PIC mapping resolves to nothing.
 */
/*
 * The ATV DSDT's _PRT, per slot (its entries wildcard the PCI function, which
 * is why this is slot-granular - the same granularity a $PIR table has).
 * Slots missing from this table are not wired on the board.
 */
static const struct {
    uint8_t slot;
    int8_t pin[4];
} atv_pirq_wiring[] = {
    /*        INTA  INTB  INTC  INTD */
    { 0x01, {  0,   -1,   -1,   -1 } },  /* PCIe root port (GPU behind it) */
    { 0x02, {  0,   -1,   -1,   -1 } },  /* "IGD"/NIC slot */
    { 0x07, {  0,   -1,   -1,   -1 } },  /* performance counters */
    { 0x1b, {  6,   -1,   -1,   -1 } },  /* HDA */
    { 0x1c, {  1,    0,    2,    3 } },  /* PCIe ports 1..4 */
    { 0x1d, {  5,    3,    2,    0 } },  /* UHCI0/1/2, EHCI */
    { 0x1e, {  6,    4,   -1,   -1 } },  /* PCI/PCI-X bridge */
    { 0x1f, {  2,    3,   -1,    0 } },  /* LPC, IDE, SATA, SMBus */
};

int piix_atv_slot_pirq(int slot, int pin)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(atv_pirq_wiring); i++) {
        if (atv_pirq_wiring[i].slot == slot) {
            return atv_pirq_wiring[i].pin[pin];
        }
    }
    return PIIX_ATV_SLOT_PIRQ_INVALID;
}
static uint8_t piix_atv_pirq_rout(PIIXState *s, int pirq)
{
    return s->dev.config[PIIX_ATV_PIRQCA + pirq];
}

/* Level of one ISA IRQ line as the OR of every PIRQ routed to it. */
static void piix_atv_update_pic(PIIXState *s, int pic_irq)
{
    PCIBus *bus = pci_get_bus(&s->dev);
    int i, level = 0;

    for (i = 0; i < PIIX_ATV_NUM_PIRQS; i++) {
        uint8_t rout = piix_atv_pirq_rout(s, i);

        if (!(rout & 0x80) && (rout & 0x0f) == pic_irq) {
            level |= pci_bus_get_irq_level(bus, i);
        }
    }
    qemu_set_irq(s->isa_irqs_in[pic_irq], level);
}

static void piix_atv_set_irq(void *opaque, int pirq, int level)
{
    PIIXState *s = opaque;
    int pic_irq = piix_atv_pirq_rout(s, pirq) & 0x0f;

    s->atv_pirq_level &= ~(1 << pirq);
    s->atv_pirq_level |= !!level << pirq;

    /* The IO-APIC wiring is fixed: PIRQ[A-H] -> GSI 16..23. */
    qemu_set_irq(s->atv_pirq_apic[pirq], level);

    if (!(piix_atv_pirq_rout(s, pirq) & 0x80) && pic_irq < ISA_NUM_IRQS) {
        piix_atv_update_pic(s, pic_irq);
    }
}

static PCIINTxRoute piix_atv_route_intx_pin_to_irq(void *opaque, int pirq)
{
    PIIXState *s = opaque;
    uint8_t rout = piix_atv_pirq_rout(s, pirq);
    PCIINTxRoute route;

    route.mode = PCI_INTX_ENABLED;
    if (!(rout & 0x80) && (rout & 0x0f) < ISA_NUM_IRQS) {
        route.irq = rout & 0x0f;
    } else {
        route.irq = PIIX_ATV_PIRQ_APIC_BASE + pirq;
    }
    return route;
}

static void piix_atv_update_routing(PIIXState *s, uint32_t address, int len)
{
    int i, pic_irq;

    if (!ranges_overlap(address, len, PIIX_ATV_PIRQCA, PIIX_ATV_NUM_PIRQS)) {
        return;
    }
    pci_bus_fire_intx_routing_notifier(pci_get_bus(&s->dev));
    for (i = 0; i < PIIX_ATV_NUM_PIRQS; i++) {
        if (!ranges_overlap(address, len, PIIX_ATV_PIRQCA + i, 1)) {
            continue;
        }
        qemu_set_irq(s->atv_pirq_apic[i],
                     !!(s->atv_pirq_level & (1 << i)));
    }
    /* QEMU's level bookkeeping is per pirq; refresh every PIC line. */
    for (pic_irq = 0; pic_irq < ISA_NUM_IRQS; pic_irq++) {
        piix_atv_update_pic(s, pic_irq);
    }
}

/*
 * $PIR (PCI IRQ Routing Table v1.0) for the wiring above.  Linux's PIR scan
 * (arch/x86/pci/irq.c) reads it straight out of the F-segment, treats the link
 * byte as a config offset ("PIIX/ICH" router) and leaves the router alone when
 * the register already names a usable IRQ.
 */
size_t piix_atv_build_pir_table(uint8_t *buf)
{
    const uint8_t irq_of[PIIX_ATV_NUM_PIRQS] = PIIX_ATV_PIRQ_DEFAULT;
    uint32_t sum = 0;
    size_t off, i;

    memset(buf, 0, PIIX_ATV_PIR_TABLE_SIZE);
    memcpy(buf, "$PIR", 4);
    buf[4] = 0x00;                          /* version 1.00 */
    buf[5] = 0x01;
    buf[6] = PIIX_ATV_PIR_TABLE_SIZE & 0xff;
    buf[7] = PIIX_ATV_PIR_TABLE_SIZE >> 8;
    buf[8] = 0;                             /* router bus */
    buf[9] = 0x1f << 3;                     /* router devfn: 00:1f.0 */
    buf[12] = PCI_VENDOR_ID_INTEL & 0xff;
    buf[13] = PCI_VENDOR_ID_INTEL >> 8;
    buf[14] = PIIX3_PIRQ_ROUTER_DEVICE & 0xff;
    buf[15] = PIIX3_PIRQ_ROUTER_DEVICE >> 8;

    for (i = 0, off = 32; i < 31; i++, off += 16) {
        int slot = i + 1;
        uint8_t *e = buf + off;
        int pin;

        e[0] = 0;                           /* bus 0 */
        e[1] = slot << 3;
        for (pin = 0; pin < 4; pin++) {
            int pirq = piix_atv_slot_pirq(slot, pin);
            uint8_t *p = e + 2 + pin * 3;

            if (pirq < 0) {
                /* Not on the board's _PRT: same i440FX swizzle the machine
                 * gives the bus, so the table never disagrees with the
                 * routing pci_bus_map_irqs() installed. */
                pirq = (pin + slot - 1) & 3;
            }
            p[0] = PIIX_ATV_PIRQCA + pirq;  /* link value = config offset */
            p[1] = (1 << irq_of[pirq]) & 0xff;
            p[2] = (1 << irq_of[pirq]) >> 8;
        }
        e[14] = slot <= 4 ? slot - 1 : 0;   /* slot number, 0 = onboard */
    }

    for (i = 0; i < PIIX_ATV_PIR_TABLE_SIZE; i++) {
        sum += buf[i];
    }
    buf[PIIX_ATV_PIR_TABLE_SIZE - 1] = (uint8_t)(0x100 - (sum & 0xff));
    return PIIX_ATV_PIR_TABLE_SIZE;
}

/*
 * Republish the $PIR table into the F-segment, but only while the firmware is
 * leaving that window alone: its DRAM pattern test paints 0xe0000-0xfffff and
 * verifies what it wrote, so a write of ours inside its write/verify pair would
 * fail its memory test outright.  One full poll period of silence therefore
 * gates every write; before the test has run, and again once it is over, the
 * window is stable and the table simply (re)appears.
 */
static void piix_atv_publish_pir(PIIXState *s)
{
    uint8_t now[PIIX_ATV_PIR_PROBE_SIZE + 16];
    bool quiet;

    address_space_read(&address_space_memory, PIIX_ATV_PIR_TABLE_ADDR,
                       MEMTXATTRS_UNSPECIFIED, now, PIIX_ATV_PIR_PROBE_SIZE);
    address_space_read(&address_space_memory, PIIX_ATV_PIR_PROBE_END,
                       MEMTXATTRS_UNSPECIFIED,
                       now + PIIX_ATV_PIR_PROBE_SIZE, 16);

    quiet = !memcmp(now, s->atv_pir_seen, sizeof(now));
    memcpy(s->atv_pir_seen, now, sizeof(now));
    if (!quiet || !memcmp(now, s->atv_pir_table, PIIX_ATV_PIR_PROBE_SIZE)) {
        return;
    }
    address_space_write(&address_space_memory, PIIX_ATV_PIR_TABLE_ADDR,
                        MEMTXATTRS_UNSPECIFIED, s->atv_pir_table,
                        sizeof(s->atv_pir_table));
}

static void piix_atv_pir_timer(void *opaque)
{
    PIIXState *s = opaque;

    piix_atv_publish_pir(s);
    timer_mod(s->atv_pir_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + PIIX_ATV_PIR_POLL_MS);
}


/* irq routing is changed. so rebuild bitmap */
static void piix_update_pci_irq_levels(PIIXState *s)
{
    PCIBus *bus = pci_get_bus(&s->dev);
    int pirq;

    s->pic_levels = 0;
    for (pirq = 0; pirq < PIIX_NUM_PIRQS; pirq++) {
        piix_set_pci_irq_level(s, pirq, pci_bus_get_irq_level(bus, pirq));
    }
}

static void piix_write_config(PCIDevice *dev, uint32_t address, uint32_t val,
                              int len)
{
    pci_default_write_config(dev, address, val, len);
    if (object_property_get_uint(qdev_get_machine(), "car-size", NULL) > 0) {
        /* Apple TV: the ATV PEI sets GEN_PMCON_2 bit 2 (SMBus reset) through
         * the MMCONFIG window and then reads it back to decide whether a
         * platform reset is still pending.  On the real ICH7 that bit is a
         * self-clearing command, but QEMU's PIIX3 latches it, which traps
         * the firmware in an endless CF9 reset loop. */
        if (ranges_overlap(address, len, 0xa2, 1)) {
            dev->config[0xa2] &= ~0x04;
        }
    }
    if (PIIX_PCI_DEVICE(dev)->atv_pirq) {
        piix_atv_update_routing(PIIX_PCI_DEVICE(dev), address, len);
        return;
    }
    if (ranges_overlap(address, len, PIIX_PIRQCA, 4)) {
        PIIXState *s = PIIX_PCI_DEVICE(dev);
        int pic_irq;

        pci_bus_fire_intx_routing_notifier(pci_get_bus(&s->dev));
        piix_update_pci_irq_levels(s);
        for (pic_irq = 0; pic_irq < ISA_NUM_IRQS; pic_irq++) {
            piix_set_irq_pic(s, pic_irq);
        }
    }
}

static void piix_reset(DeviceState *dev)
{
    PIIXState *d = PIIX_PCI_DEVICE(dev);
    uint8_t *pci_conf = d->dev.config;

    pci_conf[0x04] = 0x07; /* master, memory and I/O */
    pci_conf[0x05] = 0x00;
    pci_conf[0x06] = 0x00;
    pci_conf[0x07] = 0x02; /* PCI_status_devsel_medium */
    pci_conf[0x4c] = 0x4d;
    pci_conf[0x4e] = 0x03;
    pci_conf[0x4f] = 0x00;
    pci_conf[0x60] = 0x80;
    pci_conf[0x61] = 0x80;
    pci_conf[0x62] = 0x80;
    pci_conf[0x63] = 0x80;
    if (d->atv_pirq) {
        /* Real ICH7 reset value: PIRQ[A-D] disabled (APIC-only).  The ATV
         * machine instead programs what a BIOS on a PIC-only box would, so
         * that a guest without an IO-APIC still gets its PCI interrupts. */
        const uint8_t def[PIIX_ATV_NUM_PIRQS] = PIIX_ATV_PIRQ_DEFAULT;
        int i;

        for (i = 0; i < PIIX_ATV_NUM_PIRQS; i++) {
            pci_conf[PIIX_ATV_PIRQCA + i] = def[i];
        }
    }
    pci_conf[0x69] = 0x02;
    pci_conf[0x70] = 0x80;
    pci_conf[0x76] = 0x0c;
    pci_conf[0x77] = 0x0c;
    pci_conf[0x78] = 0x02;
    pci_conf[0x79] = 0x00;
    pci_conf[0x80] = 0x00;
    pci_conf[0x82] = 0x00;
    pci_conf[0xa0] = 0x08;
    pci_conf[0xa2] = 0x00;
    pci_conf[0xa3] = 0x00;
    pci_conf[0xa4] = 0x00;
    pci_conf[0xa5] = 0x00;
    pci_conf[0xa6] = 0x00;
    pci_conf[0xa7] = 0x00;
    pci_conf[0xa8] = 0x0f;
    pci_conf[0xaa] = 0x00;
    pci_conf[0xab] = 0x00;
    pci_conf[0xac] = 0x00;
    pci_conf[0xae] = 0x00;

    d->pic_levels = 0;
    d->rcr = 0;
}

static int piix_post_load(void *opaque, int version_id)
{
    PIIXState *s = opaque;
    int pirq;

    /*
     * Because the i8259 has not been deserialized yet, qemu_irq_raise
     * might bring the system to a different state than the saved one;
     * for example, the interrupt could be masked but the i8259 would
     * not know that yet and would trigger an interrupt in the CPU.
     *
     * Here, we update irq levels without raising the interrupt.
     * Interrupt state will be deserialized separately through the i8259.
     */
    s->pic_levels = 0;
    for (pirq = 0; pirq < PIIX_NUM_PIRQS; pirq++) {
        piix_set_pci_irq_level_internal(s, pirq,
            pci_bus_get_irq_level(pci_get_bus(&s->dev), pirq));
    }
    return 0;
}

static int piix4_post_load(void *opaque, int version_id)
{
    PIIXState *s = opaque;

    if (version_id == 2) {
        s->rcr = 0;
    }

    return piix_post_load(opaque, version_id);
}

static int piix3_pre_save(void *opaque)
{
    int i;
    PIIXState *piix3 = opaque;

    for (i = 0; i < ARRAY_SIZE(piix3->pci_irq_levels_vmstate); i++) {
        piix3->pci_irq_levels_vmstate[i] =
            pci_bus_get_irq_level(pci_get_bus(&piix3->dev), i);
    }

    return 0;
}

static bool piix3_rcr_needed(void *opaque)
{
    PIIXState *piix3 = opaque;

    return (piix3->rcr != 0);
}

static const VMStateDescription vmstate_piix3_rcr = {
    .name = "PIIX3/rcr",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = piix3_rcr_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(rcr, PIIXState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_piix3 = {
    .name = "PIIX3",
    .version_id = 3,
    .minimum_version_id = 2,
    .post_load = piix_post_load,
    .pre_save = piix3_pre_save,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, PIIXState),
        VMSTATE_INT32_ARRAY_V(pci_irq_levels_vmstate, PIIXState,
                              PIIX_NUM_PIRQS, 3),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_piix3_rcr,
        NULL
    }
};

static const VMStateDescription vmstate_piix4 = {
    .name = "PIIX4",
    .version_id = 3,
    .minimum_version_id = 2,
    .post_load = piix4_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, PIIXState),
        VMSTATE_UINT8_V(rcr, PIIXState, 3),
        VMSTATE_END_OF_LIST()
    }
};

static void rcr_write(void *opaque, hwaddr addr, uint64_t val, unsigned len)
{
    PIIXState *d = opaque;

    if (val & 4) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        return;
    }
    d->rcr = val & 2; /* keep System Reset type only */
}

static uint64_t rcr_read(void *opaque, hwaddr addr, unsigned len)
{
    PIIXState *d = opaque;

    return d->rcr;
}

static const MemoryRegionOps rcr_ops = {
    .read = rcr_read,
    .write = rcr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void pci_piix_realize(PCIDevice *dev, const char *uhci_type,
                             Error **errp)
{
    PIIXState *d = PIIX_PCI_DEVICE(dev);
    PCIBus *pci_bus = pci_get_bus(dev);
    ISABus *isa_bus;
    uint32_t irq;

    isa_bus = isa_bus_new(DEVICE(d), pci_address_space(dev),
                          pci_address_space_io(dev), errp);
    if (!isa_bus) {
        return;
    }

    memory_region_init_io(&d->rcr_mem, OBJECT(dev), &rcr_ops, d,
                          "piix-reset-control", 1);
    memory_region_add_subregion_overlap(pci_address_space_io(dev),
                                        PIIX_RCR_IOPORT, &d->rcr_mem, 1);

    /* PIC */
    if (d->has_pic) {
        qemu_irq *i8259;

        qemu_init_irq_child(OBJECT(dev), "i8259-irq", &d->i8259_irq,
                            piix_request_i8259_irq, d, 0);
        i8259 = i8259_init(isa_bus, &d->i8259_irq);

        for (size_t i = 0; i < ISA_NUM_IRQS; i++) {
            d->isa_irqs_in[i] = i8259[i];
        }

        g_free(i8259);

        qdev_init_gpio_out_named(DEVICE(dev), &d->cpu_intr, "intr", 1);
    }

    isa_bus_register_input_irqs(isa_bus, d->isa_irqs_in);

    /* PIT */
    if (d->has_pit) {
        i8254_pit_init(isa_bus, 0x40, 0, NULL);
    }

    i8257_dma_init(OBJECT(dev), isa_bus, 0);

    /* RTC */
    qdev_prop_set_int32(DEVICE(&d->rtc), "base_year", 2000);
    if (!qdev_realize(DEVICE(&d->rtc), BUS(isa_bus), errp)) {
        return;
    }
    irq = object_property_get_uint(OBJECT(&d->rtc), "irq", &error_fatal);
    isa_connect_gpio_out(ISA_DEVICE(&d->rtc), 0, irq);

    /* IDE */
    qdev_prop_set_int32(DEVICE(&d->ide), "addr", dev->devfn + 1);
    if (!qdev_realize(DEVICE(&d->ide), BUS(pci_bus), errp)) {
        return;
    }

    /* USB */
    if (d->has_usb) {
        object_initialize_child(OBJECT(dev), "uhci", &d->uhci, uhci_type);
        qdev_prop_set_int32(DEVICE(&d->uhci), "addr", dev->devfn + 2);
        if (!qdev_realize(DEVICE(&d->uhci), BUS(pci_bus), errp)) {
            return;
        }
    }

    /* Power Management */
    if (d->has_acpi) {
        object_initialize_child(OBJECT(d), "pm", &d->pm, TYPE_PIIX4_PM);
        qdev_prop_set_int32(DEVICE(&d->pm), "addr", dev->devfn + 3);
        qdev_prop_set_uint32(DEVICE(&d->pm), "smb_io_base", d->smb_io_base);
        qdev_prop_set_bit(DEVICE(&d->pm), "smm-enabled", d->smm_enabled);
        if (!qdev_realize(DEVICE(&d->pm), BUS(pci_bus), errp)) {
            return;
        }
        qdev_connect_gpio_out(DEVICE(&d->pm), 0, d->isa_irqs_in[9]);
    }

    if (d->atv_pirq) {
        /* ATV/ICH7: eight PIRQ lines, each feeding GSI 16+n as well as (when
         * the route register says so) an ISA IRQ. */
        pci_bus_irqs(pci_bus, piix_atv_set_irq, d, PIIX_ATV_NUM_PIRQS);
        pci_bus_set_route_irq_fn(pci_bus, piix_atv_route_intx_pin_to_irq);

        piix_atv_build_pir_table(d->atv_pir_table);
        d->atv_pir_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                        piix_atv_pir_timer, d);
        timer_mod(d->atv_pir_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + PIIX_ATV_PIR_POLL_MS);
    } else {
        pci_bus_irqs(pci_bus, piix_set_pci_irq, d, PIIX_NUM_PIRQS);
        pci_bus_set_route_irq_fn(pci_bus, piix_route_intx_pin_to_irq);
    }
}

static void build_pci_isa_aml(AcpiDevAmlIf *adev, Aml *scope)
{
    Aml *field;
    Aml *sb_scope = aml_scope("\\_SB");
    BusState *bus = qdev_get_child_bus(DEVICE(adev), "isa.0");

    /* PIIX PCI to ISA irq remapping */
    aml_append(scope, aml_operation_region("P40C", AML_PCI_CONFIG,
                                           aml_int(0x60), 0x04));
    /* Fields declarion has to happen *after* operation region */
    field = aml_field("PCI0.S08.P40C", AML_BYTE_ACC, AML_NOLOCK, AML_PRESERVE);
    aml_append(field, aml_named_field("PRQ0", 8));
    aml_append(field, aml_named_field("PRQ1", 8));
    aml_append(field, aml_named_field("PRQ2", 8));
    aml_append(field, aml_named_field("PRQ3", 8));
    aml_append(sb_scope, field);
    aml_append(scope, sb_scope);

    qbus_build_aml(bus, scope);
}

static void pci_piix_init(Object *obj)
{
    PIIXState *d = PIIX_PCI_DEVICE(obj);

    qdev_init_gpio_out_named(DEVICE(obj), d->isa_irqs_in, "isa-irqs",
                             ISA_NUM_IRQS);
    qdev_init_gpio_out_named(DEVICE(obj), d->atv_pirq_apic, "atv-pirq-apic",
                             PIIX_ATV_NUM_PIRQS);

    object_initialize_child(obj, "rtc", &d->rtc, TYPE_MC146818_RTC);
}

static const Property pci_piix_props[] = {
    DEFINE_PROP_UINT32("smb_io_base", PIIXState, smb_io_base, 0),
    DEFINE_PROP_BOOL("has-acpi", PIIXState, has_acpi, true),
    DEFINE_PROP_BOOL("has-pic", PIIXState, has_pic, true),
    DEFINE_PROP_BOOL("has-pit", PIIXState, has_pit, true),
    DEFINE_PROP_BOOL("has-usb", PIIXState, has_usb, true),
    DEFINE_PROP_BOOL("smm-enabled", PIIXState, smm_enabled, false),
    DEFINE_PROP_BOOL("atv-pirq", PIIXState, atv_pirq, false),
};

static void pci_piix_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    AcpiDevAmlIfClass *adevc = ACPI_DEV_AML_IF_CLASS(klass);

    k->config_write = piix_write_config;
    device_class_set_legacy_reset(dc, piix_reset);
    dc->desc        = "ISA bridge";
    dc->hotpluggable   = false;
    k->vendor_id    = PCI_VENDOR_ID_INTEL;
    k->class_id     = PCI_CLASS_BRIDGE_ISA;
    /*
     * Reason: part of PIIX southbridge, needs to be wired up by e.g.
     * pc_piix.c's pc_init1()
     */
    dc->user_creatable = false;
    device_class_set_props(dc, pci_piix_props);
    adevc->build_dev_aml = build_pci_isa_aml;
}

static const TypeInfo piix_pci_type_info = {
    .name = TYPE_PIIX_PCI_DEVICE,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PIIXState),
    .instance_init = pci_piix_init,
    .abstract = true,
    .class_init = pci_piix_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { TYPE_ACPI_DEV_AML_IF },
        { },
    },
};

static void piix3_realize(PCIDevice *dev, Error **errp)
{
    pci_piix_realize(dev, TYPE_PIIX3_USB_UHCI, errp);
}

static void piix3_init(Object *obj)
{
    PIIXState *d = PIIX_PCI_DEVICE(obj);

    object_initialize_child(obj, "ide", &d->ide, TYPE_PIIX3_IDE);
}

static void piix3_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = piix3_realize;
    /* 82371SB PIIX3 PCI-to-ISA bridge (Step A1) */
    k->device_id = PCI_DEVICE_ID_INTEL_82371SB_0;
    dc->vmsd = &vmstate_piix3;
}

static const TypeInfo piix3_info = {
    .name          = TYPE_PIIX3_DEVICE,
    .parent        = TYPE_PIIX_PCI_DEVICE,
    .instance_init = piix3_init,
    .class_init    = piix3_class_init,
};

static void piix4_realize(PCIDevice *dev, Error **errp)
{
    pci_piix_realize(dev, TYPE_PIIX4_USB_UHCI, errp);
}

static void piix4_init(Object *obj)
{
    PIIXState *s = PIIX_PCI_DEVICE(obj);

    object_initialize_child(obj, "ide", &s->ide, TYPE_PIIX4_IDE);
}

static void piix4_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = piix4_realize;
    k->device_id = PCI_DEVICE_ID_INTEL_82371AB_0;
    dc->vmsd = &vmstate_piix4;
}

static const TypeInfo piix4_info = {
    .name          = TYPE_PIIX4_PCI_DEVICE,
    .parent        = TYPE_PIIX_PCI_DEVICE,
    .instance_init = piix4_init,
    .class_init    = piix4_class_init,
};

static void piix3_register_types(void)
{
    type_register_static(&piix_pci_type_info);
    type_register_static(&piix3_info);
    type_register_static(&piix4_info);
}

type_init(piix3_register_types)
