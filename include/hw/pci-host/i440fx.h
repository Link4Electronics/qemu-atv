/*
 * QEMU i440FX North Bridge Emulation
 *
 * Copyright (c) 2006 Fabrice Bellard
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef HW_PCI_I440FX_H
#define HW_PCI_I440FX_H

#include "hw/pci/pci_device.h"
#include "hw/pci-host/pam.h"
#include "qom/object.h"

#define I440FX_HOST_PROP_PCI_TYPE "pci-type"

#define TYPE_I440FX_PCI_HOST_BRIDGE "i440FX-pcihost"
#define TYPE_I440FX_PCI_DEVICE "i440FX"

OBJECT_DECLARE_SIMPLE_TYPE(PCII440FXState, I440FX_PCI_DEVICE)

struct PCII440FXState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    PAMMemoryRegion pam_regions[PAM_REGIONS_COUNT];
    MemoryRegion smram_region;
    MemoryRegion smram, low_smram;

    /* Apple TV (car-size > 0): PCI Express MMCONFIG/ECAM window driven by
     * the MCH PCIEXBAR register (00:00.0 offset 0x48).  The 945GM firmware
     * reaches all PCI config space through it (no CF8/CFC after PEI), so
     * without it every device reads back as 0xff and the bus scan finds
     * nothing. */
    MemoryRegion atv_mmcfg;
    hwaddr atv_mmcfg_base;
    hwaddr atv_mmcfg_size;
    bool atv_mmcfg_mapped;
};

#define TYPE_IGD_PASSTHROUGH_I440FX_PCI_DEVICE "igd-passthrough-i440FX"

#endif
