/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * USB device controller shut-down when fastboot hands over.
 *
 * On "continue", and on "fastboot boot" which ends the same way, the stock
 * fastboot loop (f_007b4c) calls the USB transport's stop, f_058c90. That
 * masks the controller interrupt (SPI 0xa0) in the GIC, clears SOFTCONN (the
 * host sees the detach) and powers the PHY down (f_059fb4). The MUSB itself
 * stays as fastboot set it up (f_05830c, f_0585e0, f_059404, f_059b30):
 * L1INTM 0x2f, the EP1 QMU queues active with their descriptors in lk heap,
 * and the QMU and endpoint interrupts enabled. The next owner (Linux, or
 * Windows through UEFI) then finds a level interrupt it does not expect, and
 * the queues could go on reading or writing memory lk no longer owns (the
 * receive queue's buffers are in the download buffer, which holds the image
 * after "fastboot boot").
 *
 * Hooked in place of the PHY power-down call, we stop and clear the
 * controller first, log what it reads back, then power the PHY down as the
 * stock does. The order matters: the controller registers are not reached
 * once the PHY clock stops. SOFTCONN is already clear when we run.
 *
 * Registers from base 0x11200000, as the stock driver uses them (queue n is
 * 1..8; the stock fastboot uses queue 1 both ways): writing 4 to a queue's
 * CSR stops it, bit 15 stays set while it is active (f_059cb4); interrupt
 * status registers are cleared by writing 1s (f_058240).
 */
#include "lk_stock.h"
#include "t70.h"

#ifdef T70_HOST_TEST
extern volatile uint32_t t70_test_musb[0x1000 / 4];
#define MUSB ((volatile uint8_t *)t70_test_musb)
#else
#define MUSB ((volatile uint8_t *)0xffff000011200000ul)	/* stock lk device mapping */
#endif

#define R8(o)	(*(volatile uint8_t *)(MUSB + (o)))
#define R16(o)	(*(volatile uint16_t *)(MUSB + (o)))
#define R32(o)	(*(volatile uint32_t *)(MUSB + (o)))

#define POWER		0x001	/* bit 6 SOFTCONN */
#define INTRTX		0x002	/* endpoint interrupt status, 16 bits, write 1 to clear */
#define INTRRX		0x004
#define INTRTXE		0x006	/* endpoint interrupt enables, 16 bits */
#define INTRRXE		0x008
#define INTRUSB		0x00a	/* bus interrupt status, 8 bits */
#define INTRUSBE	0x00b
#define L1INTM		0x0a4	/* level-1 interrupt mask: what reaches the GIC */
#define DMA_INTR	0x200	/* HSDMA interrupt status, bits 0-7 */
#define DMA_CNTL(n)	(0x204 + 0x10 * (n))	/* channel n = 0..7, bit 0 enable */
#define QCR0		0x800	/* queue enables: transmit bits 16-23, receive 0-7 */
#define QCR2		0x808
#define QCR3		0x80c
#define RQCSR(n)	(0x800 + 0x10 * (n))
#define RQSAR(n)	(0x804 + 0x10 * (n))
#define TQCSR(n)	(0x9f0 + 0x10 * (n))
#define TQSAR(n)	(0x9f4 + 0x10 * (n))
#define USBGCSR		0xb00	/* queue enables: transmit bits 16-23, receive 0-7 */
#define QISAR		0xc00	/* QMU interrupt status */
#define QIMSR		0xc0c	/* QMU interrupt mask set */

#define Q_ACTIVE	0x8000
#define Q_STOP		0x4
#define Q_POLLS		10000	/* 1 us apart: 10 ms for a queue to stop */

/* QMU error and empty interrupts: status register (mask set at +0xc) and
 * its bits. */
static const struct { uint16_t status; uint32_t bits; } qmu_irqs[] = {
	{ 0xc60, 0x000000ff }, { 0xc70, 0x000000ff }, { 0xc90, 0xff00ffff },
	{ 0xca0, 0x000000ff }, { 0xcb0, 0x00ffffff }, { 0xcc0, 0x000000ff },
};

/* Stop the queue whose CSR is at csr if it is active; 1 if it stayed active. */
static int queue_stop(unsigned csr, unsigned sar, int *stopped)
{
	int polls = 0;

	if (R32(csr) & Q_ACTIVE) {
		R32(csr) = Q_STOP;
		do
			lk_delay_us(1);
		while ((R32(csr) & Q_ACTIVE) && ++polls < Q_POLLS);
		if (R32(csr) & Q_ACTIVE)
			return 1;
		(*stopped)++;
	}
	if (R32(sar))
		R32(sar) = 0;
	return 0;
}

static void ack32(unsigned reg, uint32_t bits)
{
	uint32_t s = R32(reg) & bits;

	if (s)
		R32(reg) = s;
}

void t70_usb_stop(void)
{
	int stopped = 0, stuck = 0;
	unsigned n;
	uint8_t s8;

	/* 1. no more controller interrupts */
	R8(INTRUSBE) = 0;
	R16(INTRTXE) = 0;
	R16(INTRRXE) = 0;

	/* 2. HSDMA channels */
	for (n = 0; n < 8; n++)
		if (R32(DMA_CNTL(n)) & 1)
			R32(DMA_CNTL(n)) = 0;

	/* 3. QMU queues: stop, then forget their descriptors */
	for (n = 1; n <= 8; n++) {
		stuck += queue_stop(RQCSR(n), RQSAR(n), &stopped);
		stuck += queue_stop(TQCSR(n), TQSAR(n), &stopped);
	}

	/* 4. queue enables */
	R32(USBGCSR) &= ~0x00ff00ffu;
	R32(QCR0) &= ~0x00ff00ffu;
	R32(QCR2) &= ~0xff000000u;
	R32(QCR3) &= ~0xff000000u;

	/* 5. QMU interrupts masked and cleared */
	R32(QIMSR) = 0x1e03ffff;
	for (n = 0; n < sizeof(qmu_irqs) / sizeof(qmu_irqs[0]); n++) {
		R32(qmu_irqs[n].status + 0xc) = qmu_irqs[n].bits;
		ack32(qmu_irqs[n].status, qmu_irqs[n].bits);
	}
	ack32(QISAR, 0xffffffffu);

	/* 6. nothing reaches the GIC; clear what is pending */
	R32(L1INTM) = 0;
	R16(INTRTX) = 0xffff;
	R16(INTRRX) = 0xffff;
	R8(INTRUSB) = 0xff;
	s8 = R8(DMA_INTR);
	if (s8)
		R8(DMA_INTR) = s8;

	/* 7. SOFTCONN: already clear (f_058c90); make sure */
	if (R8(POWER) & 0x40)
		R8(POWER) &= ~0x40;

	lk_dprintf("t70: usb stop: %d queues stopped, %d still active; L1INTM %x USBGCSR %x "
		   "QISAR %x TQCSR1 %x RQCSR1 %x POWER %x\n", stopped, stuck, R32(L1INTM),
		   R32(USBGCSR), R32(QISAR), R32(TQCSR(1)), R32(RQCSR(1)), R8(POWER));

	lk_usb_phy_off();
}
