/*
 * QEMU PIIX South Bridge Emulation
 *
 * Copyright (c) 2006 Fabrice Bellard
 * Copyright (c) 2018 Hervé Poussineau
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef HW_SOUTHBRIDGE_PIIX_H
#define HW_SOUTHBRIDGE_PIIX_H

#include "hw/pci/pci_device.h"
#include "hw/acpi/piix4.h"
#include "hw/ide/pci.h"
#include "hw/rtc/mc146818rtc.h"
#include "hw/usb/hcd-uhci.h"
#include "hw/core/irq.h"
#include "qemu/timer.h"

/* PIRQRC[A:D]: PIRQx Route Control Registers */
#define PIIX_PIRQCA 0x60
#define PIIX_PIRQCB 0x61
#define PIIX_PIRQCC 0x62
#define PIIX_PIRQCD 0x63

/*
 * Reset Control Register: PCI-accessible ISA-Compatible Register at address
 * 0xcf9, provided by the PCI/ISA bridge (PIIX3 PCI function 0, 8086:7000).
 */
#define PIIX_RCR_IOPORT 0xcf9

#define PIIX_NUM_PIRQS          4ULL    /* PIRQ[A-D] */

/*
 * Apple TV: the board's south bridge is an ICH7-M (82801GBM) whose LPC
 * function (00:1f.0) carries the PIRQ router used by the real firmware:
 * eight route control registers at 0x60..0x67, one per PIRQ[A-H] line.  Each
 * line reaches the IO-APIC on GSI 16+n (that is where the real DSDT's _PRT
 * points: PIRQA=16 ... PIRQG=22) and, when bit 7 of the route register is
 * clear, the 8259 line named in its low nibble as well.
 */
#define PIIX_ATV_PIRQCA         0x60
#define PIIX_ATV_NUM_PIRQS      8
#define PIIX_ATV_PIRQ_APIC_BASE 16

/*
 * Device ID the $PIR table advertises for the interrupt router.  It lives in
 * 00:1f.0, which the ATV machine presents as a PIIX3 (8086:7000); Linux's
 * intel_router_probe() accepts that one as "PIIX/ICH", so the link values in
 * the table are plain PCI config offsets, exactly like a PIIX IDE box.
 */
#define PIIX3_PIRQ_ROUTER_DEVICE 0x7000

/*
 * PIC-mode programming the ATV machine applies at reset: what a BIOS for a
 * PIC-only OS would write (the real firmware instead leaves every link
 * disabled, i.e. APIC-only, which is precisely why a guest that has not
 * discovered an IO-APIC gets no PCI interrupts at all).  Every value here is
 * an ISA IRQ that no other device of the machine claims.
 */
#define PIIX_ATV_PIRQ_DEFAULT { 10, 11, 5, 7, 3, 11, 10, 5 }

/*
 * PIRQ link each PCI slot's INTx is wired to, taken from the ATV1 firmware's
 * own DSDT _PRT (the \GPIC branch lists GSI 16..23, i.e. PIRQ[A-H]):
 *   00:01.0 A -> PIRQA   00:1b.0 A -> PIRQG   00:1d.0 A -> PIRQF
 *   00:02.0 A -> PIRQA   00:1c.0 B -> PIRQA   00:1d.1 B -> PIRQD
 *   00:07.0 A -> PIRQA   00:1c.0 C -> PIRQC   00:1d.2 C -> PIRQC
 *   00:1e.0 A -> PIRQG   00:1c.0 D -> PIRQD   00:1d.3 D -> PIRQA
 *   00:1e.0 B -> PIRQE   00:1f.0 A -> PIRQC   00:1f.0 B -> PIRQD
 *   00:1f.0 D -> PIRQA
 * Slots the firmware does not route (i.e. everything the ATV does not have)
 * keep the standard i440FX swizzle, (slot + pin - 1) & 3.
 */
#define PIIX_ATV_SLOT_PIRQ_INVALID (-1)

/* PIRQ link (0 = PIRQ[A] ... 6 = PIRQ[G]) slot @pin is wired to, or
 * PIIX_ATV_SLOT_PIRQ_INVALID when the board does not route it. */
int piix_atv_slot_pirq(int slot, int pin);

/* Build the $PIR (PCI IRQ Routing Table v1.0) matching the above wiring.
 * Returns the table size in bytes; @buf must hold PIIX_ATV_PIR_TABLE_SIZE. */
#define PIIX_ATV_PIR_TABLE_SIZE  (32 + 31 * 16)

/*
 * Where the router publishes that table: the top of the F-segment, which every
 * x86 OS scans for "$PIR".
 *
 * It cannot be written once at reset.  The firmware's early DRAM pattern test
 * treats 0xe0000-0xfffff as the low end of the memory it is sizing and paints
 * its pattern over the whole window (that is the same test that needed the
 * writable isa-bios-ram mapping in x86-common.c), and it does so *after* it has
 * posted every POST code; a table placed there at machine init is gone before
 * any OS could look for it.  Instead the window is watched and the table is
 * (re)written the moment the firmware stops writing to it, which is exactly the
 * "POST is over, hand over to the boot loader" point at which a legacy BIOS
 * would have published it.
 */
#define PIIX_ATV_PIR_TABLE_ADDR  0xf0000

/* Published table page history: the head of the table window plus the last
 * bytes of the F-segment (a pass that only touches high memory still shows up). */
#define PIIX_ATV_PIR_PROBE_SIZE  0x100
#define PIIX_ATV_PIR_PROBE_END   (0x100000 - 16)
#define PIIX_ATV_PIR_POLL_MS     100

struct PIIXState {
    PCIDevice dev;

    /*
     * bitmap to track pic levels.
     * The pic level is the logical OR of all the PCI irqs mapped to it
     * So one PIC level is tracked by PIIX_NUM_PIRQS bits.
     *
     * PIRQ is mapped to PIC pins, we track it by
     * PIIX_NUM_PIRQS * ISA_NUM_IRQS = 64 bits with
     * pic_irq * PIIX_NUM_PIRQS + pirq
     */
#if ISA_NUM_IRQS * PIIX_NUM_PIRQS > 64
#error "unable to encode pic state in 64bit in pic_levels."
#endif
    uint64_t pic_levels;

    qemu_irq cpu_intr;
    qemu_irq isa_irqs_in[ISA_NUM_IRQS];

    IRQState i8259_irq;

    /* This member isn't used. Just for save/load compatibility */
    int32_t pci_irq_levels_vmstate[PIIX_NUM_PIRQS];

    MC146818RtcState rtc;
    PCIIDEState ide;
    UHCIState uhci;
    PIIX4PMState pm;

    uint32_t smb_io_base;

    /* Reset Control Register contents */
    uint8_t rcr;

    /* IO memory region for Reset Control Register (PIIX_RCR_IOPORT) */
    MemoryRegion rcr_mem;

    bool has_acpi;
    bool has_pic;
    bool has_pit;
    bool has_usb;
    bool smm_enabled;

    /* ATV (ICH7-M) PIRQ router: see PIIX_ATV_* above */
    bool atv_pirq;
    qemu_irq atv_pirq_apic[PIIX_ATV_NUM_PIRQS];
    uint8_t atv_pirq_level;

    /* $PIR publication into the F-segment (see PIIX_ATV_PIR_TABLE_ADDR) */
    QEMUTimer *atv_pir_timer;
    uint8_t atv_pir_table[PIIX_ATV_PIR_TABLE_SIZE];
    uint8_t atv_pir_seen[PIIX_ATV_PIR_PROBE_SIZE + 16];
};

#define TYPE_PIIX_PCI_DEVICE "pci-piix"
OBJECT_DECLARE_SIMPLE_TYPE(PIIXState, PIIX_PCI_DEVICE)

#define TYPE_PIIX3_DEVICE "PIIX3"
#define TYPE_PIIX4_PCI_DEVICE "piix4-isa"

#endif
