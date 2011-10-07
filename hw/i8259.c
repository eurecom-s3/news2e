/*
 * QEMU 8259 interrupt controller emulation
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
#include "hw.h"
#include "pc.h"
#include "isa.h"
#include "monitor.h"
#include "qemu-timer.h"

/* debug PIC */
//#define DEBUG_PIC

#ifdef DEBUG_PIC
#define DPRINTF(fmt, ...)                                       \
    do { printf("pic: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...)
#endif

//#define DEBUG_IRQ_LATENCY
//#define DEBUG_IRQ_COUNT

typedef struct PicState {
    uint8_t last_irr; /* edge detection */
    uint8_t irr; /* interrupt request register */
    uint8_t imr; /* interrupt mask register */
    uint8_t isr; /* interrupt service register */
    uint8_t priority_add; /* highest irq priority */
    uint8_t irq_base;
    uint8_t read_reg_select;
    uint8_t poll;
    uint8_t special_mask;
    uint8_t init_state;
    uint8_t auto_eoi;
    uint8_t rotate_on_auto_eoi;
    uint8_t special_fully_nested_mode;
    uint8_t init4; /* true if 4 byte init */
    uint8_t single_mode; /* true if slave pic is not initialized */
    uint8_t elcr; /* PIIX edge/trigger selection*/
    uint8_t elcr_mask;
    qemu_irq int_out;
    PicState2 *pics_state;
    MemoryRegion base_io;
    MemoryRegion elcr_io;
} PicState;

struct PicState2 {
    /* 0 is master pic, 1 is slave pic */
    /* XXX: better separation between the two pics */
    PicState pics[2];
    void *irq_request_opaque;
};

#if defined(DEBUG_PIC) || defined (DEBUG_IRQ_COUNT)
static int irq_level[16];
#endif
#ifdef DEBUG_IRQ_COUNT
static uint64_t irq_count[16];
#endif
PicState2 *isa_pic;

/* return the highest priority found in mask (highest = smallest
   number). Return 8 if no irq */
static int get_priority(PicState *s, int mask)
{
    int priority;
    if (mask == 0)
        return 8;
    priority = 0;
    while ((mask & (1 << ((priority + s->priority_add) & 7))) == 0)
        priority++;
    return priority;
}

/* return the pic wanted interrupt. return -1 if none */
static int pic_get_irq(PicState *s)
{
    int mask, cur_priority, priority;

    mask = s->irr & ~s->imr;
    priority = get_priority(s, mask);
    if (priority == 8)
        return -1;
    /* compute current priority. If special fully nested mode on the
       master, the IRQ coming from the slave is not taken into account
       for the priority computation. */
    mask = s->isr;
    if (s->special_mask)
        mask &= ~s->imr;
    if (s->special_fully_nested_mode && s == &s->pics_state->pics[0])
        mask &= ~(1 << 2);
    cur_priority = get_priority(s, mask);
    if (priority < cur_priority) {
        /* higher priority found: an irq should be generated */
        return (priority + s->priority_add) & 7;
    } else {
        return -1;
    }
}

/* Update INT output. Must be called every time the output may have changed. */
static void pic_update_irq(PicState *s)
{
    int irq;

    irq = pic_get_irq(s);
    if (irq >= 0) {
        DPRINTF("pic%d: imr=%x irr=%x padd=%d\n",
                s == &s->pics_state->pics[0] ? 0 : 1, s->imr, s->irr,
                s->priority_add);
        qemu_irq_raise(s->int_out);
    } else {
        qemu_irq_lower(s->int_out);
    }
}

/* set irq level. If an edge is detected, then the IRR is set to 1 */
static void pic_set_irq1(PicState *s, int irq, int level)
{
    int mask;
    mask = 1 << irq;
    if (s->elcr & mask) {
        /* level triggered */
        if (level) {
            s->irr |= mask;
            s->last_irr |= mask;
        } else {
            s->irr &= ~mask;
            s->last_irr &= ~mask;
        }
    } else {
        /* edge triggered */
        if (level) {
            if ((s->last_irr & mask) == 0) {
                s->irr |= mask;
            }
            s->last_irr |= mask;
        } else {
            s->last_irr &= ~mask;
        }
    }
    pic_update_irq(s);
}

#ifdef DEBUG_IRQ_LATENCY
int64_t irq_time[16];
#endif

static void i8259_set_irq(void *opaque, int irq, int level)
{
    PicState2 *s = opaque;

#if defined(DEBUG_PIC) || defined(DEBUG_IRQ_COUNT)
    if (level != irq_level[irq]) {
        DPRINTF("i8259_set_irq: irq=%d level=%d\n", irq, level);
        irq_level[irq] = level;
#ifdef DEBUG_IRQ_COUNT
	if (level == 1)
	    irq_count[irq]++;
#endif
    }
#endif
#ifdef DEBUG_IRQ_LATENCY
    if (level) {
        irq_time[irq] = qemu_get_clock_ns(vm_clock);
    }
#endif
    pic_set_irq1(&s->pics[irq >> 3], irq & 7, level);
}

/* acknowledge interrupt 'irq' */
static void pic_intack(PicState *s, int irq)
{
    if (s->auto_eoi) {
        if (s->rotate_on_auto_eoi)
            s->priority_add = (irq + 1) & 7;
    } else {
        s->isr |= (1 << irq);
    }
    /* We don't clear a level sensitive interrupt here */
    if (!(s->elcr & (1 << irq)))
        s->irr &= ~(1 << irq);
    pic_update_irq(s);
}

int pic_read_irq(PicState2 *s)
{
    int irq, irq2, intno;

    irq = pic_get_irq(&s->pics[0]);
    if (irq >= 0) {
        if (irq == 2) {
            irq2 = pic_get_irq(&s->pics[1]);
            if (irq2 >= 0) {
                pic_intack(&s->pics[1], irq2);
            } else {
                /* spurious IRQ on slave controller */
                irq2 = 7;
            }
            intno = s->pics[1].irq_base + irq2;
        } else {
            intno = s->pics[0].irq_base + irq;
        }
        pic_intack(&s->pics[0], irq);
    } else {
        /* spurious IRQ on host controller */
        irq = 7;
        intno = s->pics[0].irq_base + irq;
    }

#if defined(DEBUG_PIC) || defined(DEBUG_IRQ_LATENCY)
    if (irq == 2) {
        irq = irq2 + 8;
    }
#endif
#ifdef DEBUG_IRQ_LATENCY
    printf("IRQ%d latency=%0.3fus\n",
           irq,
           (double)(qemu_get_clock_ns(vm_clock) -
                    irq_time[irq]) * 1000000.0 / get_ticks_per_sec());
#endif
    DPRINTF("pic_interrupt: irq=%d\n", irq);
    return intno;
}

static void pic_init_reset(PicState *s)
{
    s->last_irr = 0;
    s->irr = 0;
    s->imr = 0;
    s->isr = 0;
    s->priority_add = 0;
    s->irq_base = 0;
    s->read_reg_select = 0;
    s->poll = 0;
    s->special_mask = 0;
    s->init_state = 0;
    s->auto_eoi = 0;
    s->rotate_on_auto_eoi = 0;
    s->special_fully_nested_mode = 0;
    s->init4 = 0;
    s->single_mode = 0;
    /* Note: ELCR is not reset */
    pic_update_irq(s);
}

static void pic_reset(void *opaque)
{
    PicState *s = opaque;

    pic_init_reset(s);
    s->elcr = 0;
}

static void pic_ioport_write(void *opaque, target_phys_addr_t addr64,
                             uint64_t val64, unsigned size)
{
    PicState *s = opaque;
    uint32_t addr = addr64;
    uint32_t val = val64;
    int priority, cmd, irq;

    DPRINTF("write: addr=0x%02x val=0x%02x\n", addr, val);
    if (addr == 0) {
        if (val & 0x10) {
            pic_init_reset(s);
            s->init_state = 1;
            s->init4 = val & 1;
            s->single_mode = val & 2;
            if (val & 0x08)
                hw_error("level sensitive irq not supported");
        } else if (val & 0x08) {
            if (val & 0x04)
                s->poll = 1;
            if (val & 0x02)
                s->read_reg_select = val & 1;
            if (val & 0x40)
                s->special_mask = (val >> 5) & 1;
        } else {
            cmd = val >> 5;
            switch(cmd) {
            case 0:
            case 4:
                s->rotate_on_auto_eoi = cmd >> 2;
                break;
            case 1: /* end of interrupt */
            case 5:
                priority = get_priority(s, s->isr);
                if (priority != 8) {
                    irq = (priority + s->priority_add) & 7;
                    s->isr &= ~(1 << irq);
                    if (cmd == 5)
                        s->priority_add = (irq + 1) & 7;
                    pic_update_irq(s);
                }
                break;
            case 3:
                irq = val & 7;
                s->isr &= ~(1 << irq);
                pic_update_irq(s);
                break;
            case 6:
                s->priority_add = (val + 1) & 7;
                pic_update_irq(s);
                break;
            case 7:
                irq = val & 7;
                s->isr &= ~(1 << irq);
                s->priority_add = (irq + 1) & 7;
                pic_update_irq(s);
                break;
            default:
                /* no operation */
                break;
            }
        }
    } else {
        switch(s->init_state) {
        case 0:
            /* normal mode */
            s->imr = val;
            pic_update_irq(s);
            break;
        case 1:
            s->irq_base = val & 0xf8;
            s->init_state = s->single_mode ? (s->init4 ? 3 : 0) : 2;
            break;
        case 2:
            if (s->init4) {
                s->init_state = 3;
            } else {
                s->init_state = 0;
            }
            break;
        case 3:
            s->special_fully_nested_mode = (val >> 4) & 1;
            s->auto_eoi = (val >> 1) & 1;
            s->init_state = 0;
            break;
        }
    }
}

static uint32_t pic_poll_read(PicState *s)
{
    int ret;

    ret = pic_get_irq(s);
    if (ret >= 0) {
        bool slave = (s == &isa_pic->pics[1]);

        if (slave) {
            s->pics_state->pics[0].isr &= ~(1 << 2);
            s->pics_state->pics[0].irr &= ~(1 << 2);
        }
        s->irr &= ~(1 << ret);
        s->isr &= ~(1 << ret);
        if (slave || ret != 2) {
            pic_update_irq(s);
        }
    } else {
        ret = 0x07;
    }

    return ret;
}

static uint64_t pic_ioport_read(void *opaque, target_phys_addr_t addr1,
                                unsigned size)
{
    PicState *s = opaque;
    unsigned int addr = addr1;
    int ret;

    if (s->poll) {
        ret = pic_poll_read(s);
        s->poll = 0;
    } else {
        if (addr == 0) {
            if (s->read_reg_select)
                ret = s->isr;
            else
                ret = s->irr;
        } else {
            ret = s->imr;
        }
    }
    DPRINTF("read: addr=0x%02x val=0x%02x\n", addr, ret);
    return ret;
}

/* memory mapped interrupt status */
/* XXX: may be the same than pic_read_irq() */
uint32_t pic_intack_read(PicState2 *s)
{
    int ret;

    ret = pic_poll_read(&s->pics[0]);
    if (ret == 2)
        ret = pic_poll_read(&s->pics[1]) + 8;
    /* Prepare for ISR read */
    s->pics[0].read_reg_select = 1;

    return ret;
}

int pic_get_output(PicState2 *s)
{
    return (pic_get_irq(&s->pics[0]) >= 0);
}

static void elcr_ioport_write(void *opaque, target_phys_addr_t addr,
                              uint64_t val, unsigned size)
{
    PicState *s = opaque;
    s->elcr = val & s->elcr_mask;
}

static uint64_t elcr_ioport_read(void *opaque, target_phys_addr_t addr,
                                 unsigned size)
{
    PicState *s = opaque;
    return s->elcr;
}

static const VMStateDescription vmstate_pic = {
    .name = "i8259",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_UINT8(last_irr, PicState),
        VMSTATE_UINT8(irr, PicState),
        VMSTATE_UINT8(imr, PicState),
        VMSTATE_UINT8(isr, PicState),
        VMSTATE_UINT8(priority_add, PicState),
        VMSTATE_UINT8(irq_base, PicState),
        VMSTATE_UINT8(read_reg_select, PicState),
        VMSTATE_UINT8(poll, PicState),
        VMSTATE_UINT8(special_mask, PicState),
        VMSTATE_UINT8(init_state, PicState),
        VMSTATE_UINT8(auto_eoi, PicState),
        VMSTATE_UINT8(rotate_on_auto_eoi, PicState),
        VMSTATE_UINT8(special_fully_nested_mode, PicState),
        VMSTATE_UINT8(init4, PicState),
        VMSTATE_UINT8(single_mode, PicState),
        VMSTATE_UINT8(elcr, PicState),
        VMSTATE_END_OF_LIST()
    }
};

static const MemoryRegionOps pic_base_ioport_ops = {
    .read = pic_ioport_read,
    .write = pic_ioport_write,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static const MemoryRegionOps pic_elcr_ioport_ops = {
    .read = elcr_ioport_read,
    .write = elcr_ioport_write,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* XXX: add generic master/slave system */
static void pic_init(int io_addr, int elcr_addr, PicState *s, qemu_irq int_out)
{
    s->int_out = int_out;

    memory_region_init_io(&s->base_io, &pic_base_ioport_ops, s, "pic", 2);
    memory_region_init_io(&s->elcr_io, &pic_elcr_ioport_ops, s, "elcr", 1);

    isa_register_ioport(NULL, &s->base_io, io_addr);
    if (elcr_addr >= 0) {
        isa_register_ioport(NULL, &s->elcr_io, elcr_addr);
    }

    vmstate_register(NULL, io_addr, &vmstate_pic, s);
    qemu_register_reset(pic_reset, s);
}

void pic_info(Monitor *mon)
{
    int i;
    PicState *s;

    if (!isa_pic)
        return;

    for(i=0;i<2;i++) {
        s = &isa_pic->pics[i];
        monitor_printf(mon, "pic%d: irr=%02x imr=%02x isr=%02x hprio=%d "
                       "irq_base=%02x rr_sel=%d elcr=%02x fnm=%d\n",
                       i, s->irr, s->imr, s->isr, s->priority_add,
                       s->irq_base, s->read_reg_select, s->elcr,
                       s->special_fully_nested_mode);
    }
}

void irq_info(Monitor *mon)
{
#ifndef DEBUG_IRQ_COUNT
    monitor_printf(mon, "irq statistic code not compiled.\n");
#else
    int i;
    int64_t count;

    monitor_printf(mon, "IRQ statistics:\n");
    for (i = 0; i < 16; i++) {
        count = irq_count[i];
        if (count > 0)
            monitor_printf(mon, "%2d: %" PRId64 "\n", i, count);
    }
#endif
}

qemu_irq *i8259_init(qemu_irq parent_irq)
{
    qemu_irq *irqs;
    PicState2 *s;

    s = g_malloc0(sizeof(PicState2));
    irqs = qemu_allocate_irqs(i8259_set_irq, s, 16);
    pic_init(0x20, 0x4d0, &s->pics[0], parent_irq);
    pic_init(0xa0, 0x4d1, &s->pics[1], irqs[2]);
    s->pics[0].elcr_mask = 0xf8;
    s->pics[1].elcr_mask = 0xde;
    s->pics[0].pics_state = s;
    s->pics[1].pics_state = s;
    isa_pic = s;
    return irqs;
}
