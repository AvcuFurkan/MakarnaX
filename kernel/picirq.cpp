/*
 * Bu dosya xv6'dan alindi
 */
/*
 * Copyright (c) 2006-2009 Frans Kaashoek, Robert Morris, Russ Cox,
 *                        Massachusetts Institute of Technology
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <kernel/kernel.h>
#include <kernel/trap.h>
#include <asm/io.h>

/** Master (IRQs 0-7) */
#define IO_PIC1 0x20
/** Slave (IRQs 8-15) */
#define IO_PIC2 0xA0

#define IRQ_SLAVE       2       // IRQ at which slave connects to master

// Current IRQ mask.
// Initial IRQ mask has interrupt 2 enabled (for slave 8259A).
static uint16_t irqmask = 0xFFFF & ~(1<<IRQ_SLAVE);

static void pic_setmask(uint16_t mask) {
	irqmask = mask;
	outb(IO_PIC1+1, mask);
	outb(IO_PIC2+1, mask >> 8);
}

void pic_enable(int irq) {
	pic_setmask(irqmask & ~(1<<irq));
}

void picirq_init() {
	// mask all interrupts
	outb(IO_PIC1+1, 0xFF);
	outb(IO_PIC2+1, 0xFF);

	// Set up master (8259A-1)

	// ICW1:  0001g0hi
	//    g:  0 = edge triggering, 1 = level triggering
	//    h:  0 = cascaded PICs, 1 = master only
	//    i:  0 = no ICW4, 1 = ICW4 required
	outb(IO_PIC1, 0x11);

	// ICW2:  Vector offset
	outb(IO_PIC1+1, T_IRQ0);

	// ICW3:  (master PIC) bit mask of IR lines connected to slaves
	//        (slave PIC) 3-bit # of slave's connection to master
	outb(IO_PIC1+1, 1<<IRQ_SLAVE);

	// ICW4:  000nbmap
	//    n:  1 = special fully nested mode
	//    b:  1 = buffered mode
	//    m:  0 = slave PIC, 1 = master PIC
	//      (ignored when b is 0, as the master/slave role
	//      can be hardwired).
	//    a:  1 = Automatic EOI mode
	//    p:  0 = MCS-80/85 mode, 1 = intel x86 mode
	outb(IO_PIC1+1, 0x3);

	// Set up slave (8259A-2)
	outb(IO_PIC2, 0x11);                  // ICW1
	outb(IO_PIC2+1, T_IRQ0 + 8);      // ICW2
	outb(IO_PIC2+1, IRQ_SLAVE);           // ICW3
	// NB Automatic EOI mode doesn't tend to work on the slave.
	// Linux source code says it's "to be investigated".
	outb(IO_PIC2+1, 0x3);                 // ICW4

	// OCW3:  0ef01prs
	//   ef:  0x = NOP, 10 = clear specific mask, 11 = set specific mask
	//    p:  0 = no polling, 1 = polling mode
	//   rs:  0x = NOP, 10 = read IRR, 11 = read ISR
	outb(IO_PIC1, 0x68);             // clear specific mask
	outb(IO_PIC1, 0x0a);             // read IRR by default

	outb(IO_PIC2, 0x68);             // OCW3
	outb(IO_PIC2, 0x0a);             // OCW3

	if(irqmask != 0xFFFF)
		pic_setmask(irqmask);

	print_info(">> picirq_init OK\n");
}
