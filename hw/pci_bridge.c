/*
 * QEMU PCI bus manager
 *
 * Copyright (c) 2004 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to dea

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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM

 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
/*
 * split out from pci.c
 * Copyright (c) 2010 Isaku Yamahata <yamahata at valinux co jp>
 *                    VA Linux Systems Japan K.K.
 */

#include "pci_bridge.h"
#include "pci_internals.h"

/* Accessor function to get parent bridge device from pci bus. */
PCIDevice *pci_bridge_get_device(PCIBus *bus)
{
    return bus->parent_dev;
}

/* Accessor function to get secondary bus from pci-to-pci bridge device */
PCIBus *pci_bridge_get_sec_bus(PCIBridge *br)
{
    return &br->sec_bus;
}

static uint32_t pci_config_get_io_base(const PCIDevice *d,
                                       uint32_t base, uint32_t base_upper16)
{
    uint32_t val;

    val = ((uint32_t)d->config[base] & PCI_IO_RANGE_MASK) << 8;
    if (d->config[base] & PCI_IO_RANGE_TYPE_32) {
        val |= (uint32_t)pci_get_word(d->config + base_upper16) << 16;
    }
    return val;
}

static pcibus_t pci_config_get_memory_base(const PCIDevice *d, uint32_t base)
{
    return ((pcibus_t)pci_get_word(d->config + base) & PCI_MEMORY_RANGE_MASK)
        << 16;
}

static pcibus_t pci_config_get_pref_base(const PCIDevice *d,
                                         uint32_t base, uint32_t upper)
{
    pcibus_t tmp;
    pcibus_t val;

    tmp = (pcibus_t)pci_get_word(d->config + base);
    val = (tmp & PCI_PREF_RANGE_MASK) << 16;
    if (tmp & PCI_PREF_RANGE_TYPE_64) {
        val |= (pcibus_t)pci_get_long(d->config + upper) << 32;
    }
    return val;
}

/* accessor function to get bridge filtering base address */
pcibus_t pci_bridge_get_base(const PCIDevice *bridge, uint8_t type)
{
    pcibus_t base;
    if (type & PCI_BASE_ADDRESS_SPACE_IO) {
        base = pci_config_get_io_base(bridge,
                                      PCI_IO_BASE, PCI_IO_BASE_UPPER16);
    } else {
        if (type & PCI_BASE_ADDRESS_MEM_PREFETCH) {
            base = pci_config_get_pref_base(
                bridge, PCI_PREF_MEMORY_BASE, PCI_PREF_BASE_UPPER32);
        } else {
            base = pci_config_get_memory_base(bridge, PCI_MEMORY_BASE);
        }
    }

    return base;
}

/* accessor funciton to get bridge filtering limit */
pcibus_t pci_bridge_get_limit(const PCIDevice *bridge, uint8_t type)
{
    pcibus_t limit;
    if (type & PCI_BASE_ADDRESS_SPACE_IO) {
        limit = pci_config_get_io_base(bridge,
                                      PCI_IO_LIMIT, PCI_IO_LIMIT_UPPER16);
        limit |= 0xfff;         /* PCI bridge spec 3.2.5.6. */
    } else {
        if (type & PCI_BASE_ADDRESS_MEM_PREFETCH) {
            limit = pci_config_get_pref_base(
                bridge, PCI_PREF_MEMORY_LIMIT, PCI_PREF_LIMIT_UPPER32);
        } else {
            limit = pci_config_get_memory_base(bridge, PCI_MEMORY_LIMIT);
        }
        limit |= 0xfffff;       /* PCI bridge spec 3.2.5.{1, 8}. */
    }
    return limit;
}

/* default write_config function for PCI-to-PCI bridge */
void pci_bridge_write_config(PCIDevice *d,
                             uint32_t address, uint32_t val, int len)
{
    pci_default_write_config(d, address, val, len);

    if (/* io base/limit */
        ranges_overlap(address, len, PCI_IO_BASE, 2) ||

        /* memory base/limit, prefetchable base/limit and
           io base/limit upper 16 */
        ranges_overlap(address, len, PCI_MEMORY_BASE, 20)) {
        PCIBridge *s = container_of(d, PCIBridge, dev);
        pci_bridge_update_mappings(&s->sec_bus);
    }
}

/* reset bridge specific configuration registers */
void pci_bridge_reset_reg(PCIDevice *dev)
{
    uint8_t *conf = dev->config;

    conf[PCI_PRIMARY_BUS] = 0;
    conf[PCI_SECONDARY_BUS] = 0;
    conf[PCI_SUBORDINATE_BUS] = 0;
    conf[PCI_SEC_LATENCY_TIMER] = 0;

    conf[PCI_IO_BASE] = 0;
    conf[PCI_IO_LIMIT] = 0;
    pci_set_word(conf + PCI_MEMORY_BASE, 0);
    pci_set_word(conf + PCI_MEMORY_LIMIT, 0);
    pci_set_word(conf + PCI_PREF_MEMORY_BASE, 0);
    pci_set_word(conf + PCI_PREF_MEMORY_LIMIT, 0);
    pci_set_word(conf + PCI_PREF_BASE_UPPER32, 0);
    pci_set_word(conf + PCI_PREF_LIMIT_UPPER32, 0);

    pci_set_word(conf + PCI_BRIDGE_CONTROL, 0);
}

/* default reset function for PCI-to-PCI bridge */
void pci_bridge_reset(DeviceState *qdev)
{
    PCIDevice *dev = DO_UPCAST(PCIDevice, qdev, qdev);
    pci_bridge_reset_reg(dev);
}

/* default qdev initialization function for PCI-to-PCI bridge */
int pci_bridge_initfn(PCIDevice *dev)
{
    PCIBus *parent = dev->bus;
    PCIBridge *br = DO_UPCAST(PCIBridge, dev, dev);
    PCIBus *sec_bus = &br->sec_bus;

    pci_set_word(dev->config + PCI_STATUS,
                 PCI_STATUS_66MHZ | PCI_STATUS_FAST_BACK);
    pci_config_set_class(dev->config, PCI_CLASS_BRIDGE_PCI);
    dev->config[PCI_HEADER_TYPE] =
        (dev->config[PCI_HEADER_TYPE] & PCI_HEADER_TYPE_MULTI_FUNCTION) |
        PCI_HEADER_TYPE_BRIDGE;
    pci_set_word(dev->config + PCI_SEC_STATUS,
                 PCI_STATUS_66MHZ | PCI_STATUS_FAST_BACK);

    qbus_create_inplace(&sec_bus->qbus, &pci_bus_info, &dev->qdev,
                        br->bus_name);
    sec_bus->parent_dev = dev;
    sec_bus->map_irq = br->map_irq;

    QLIST_INIT(&sec_bus->child);
    QLIST_INSERT_HEAD(&parent->child, sec_bus, sibling);
    return 0;
}

/* default qdev clean up function for PCI-to-PCI bridge */
int pci_bridge_exitfn(PCIDevice *pci_dev)
{
    PCIBridge *s = DO_UPCAST(PCIBridge, dev, pci_dev);
    assert(QLIST_EMPTY(&s->sec_bus.child));
    QLIST_REMOVE(&s->sec_bus, sibling);
    /* qbus_free() is called automatically by qdev_free() */
    return 0;
}

/*
 * before qdev initialization(qdev_init()), this function sets bus_name and
 * map_irq callback which are necessry for pci_bridge_initfn() to
 * initialize bus.
 */
void pci_bridge_map_irq(PCIBridge *br, const char* bus_name,
                        pci_map_irq_fn map_irq)
{
    br->map_irq = map_irq;
    br->bus_name = bus_name;
}
