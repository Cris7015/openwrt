// SPDX-License-Identifier: GPL-2.0-only
/*
 * EN751221 watchdog, for catching lockups that leave no trace.
 *
 * The SoC block at 0x1fbf0100 counts down and resets the chip when it reaches
 * zero unless something writes to its reload register.  A kernel timer does
 * that here, which means the reset only happens when the machine can no longer
 * run timers at all -- exactly the failure this is meant to catch.
 *
 * The reset is warm, so DRAM keeps its contents and the ramoops region set up
 * in the device tree survives it: after the board comes back, whatever the
 * kernel printed just before dying is waiting in /sys/fs/pstore.  That is the
 * point.  A lockup where both hardware threads are stuck in interrupt context
 * cannot be reported by any of the software detectors, because all of them
 * need a timer to fire.
 *
 * Register layout and the counter rate come from the vendor's 2.6.36 tree
 * (arch/mips/ralink/time2.c, tcwdog.c): the threshold is expressed in units of
 * SYS_HCLK * 500 per millisecond, with SYS_HCLK = 133, so 66_500_000 per second.
 */

#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/timer.h>

#define EN75_WDT_BASE		0x1fbf0100
#define EN75_WDT_SIZE		0x40

#define REG_TIMER_CTL		0x00
#define REG_WDOG_THSLD		0x34
#define REG_WDOG_RLD		0x38

/*
 * The vendor sets both of these in one write:
 *	word &= 0xfdffffdf;
 *	word |= (tick_enable << 5) | (watchdog_enable << 25);
 * Enabling the watchdog alone leaves it armed but never counting, so the
 * tick bit has to go with it.
 */
#define TIMER_CTL_TICK_EN	BIT(5)
#define TIMER_CTL_WDOG_EN	BIT(25)
#define TIMER_CTL_KEEP		0xfdffffdfu

/* SYS_HCLK(133) * 500 per ms */
#define WDT_TICKS_PER_SEC	66500000u

static unsigned int timeout = 20;
module_param(timeout, uint, 0444);
MODULE_PARM_DESC(timeout, "seconds without a kick before the SoC resets");

static unsigned int kick_ms = 4000;
module_param(kick_ms, uint, 0644);
MODULE_PARM_DESC(kick_ms, "interval between kicks; 0 stops kicking, so the board resets");

static void __iomem *base;
static struct timer_list kick_timer;

static void en75_wdt_kick(void)
{
	iowrite32(1, base + REG_WDOG_RLD);
}

static void en75_wdt_kick_timer(struct timer_list *t)
{
	unsigned int ms = READ_ONCE(kick_ms);

	if (!ms) {
		pr_warn("en75-wdt: kicking disabled, the board will reset in a moment\n");
		return;
	}

	en75_wdt_kick();
	mod_timer(&kick_timer, jiffies + msecs_to_jiffies(ms));
}

static int __init en75_wdt_init(void)
{
	u32 ctl;

	if (timeout < 2 || timeout > 60) {
		pr_err("en75-wdt: timeout %u out of range (2-60)\n", timeout);
		return -EINVAL;
	}

	base = ioremap(EN75_WDT_BASE, EN75_WDT_SIZE);
	if (!base)
		return -ENOMEM;

	iowrite32(timeout * WDT_TICKS_PER_SEC, base + REG_WDOG_THSLD);
	en75_wdt_kick();

	ctl = ioread32(base + REG_TIMER_CTL);
	iowrite32((ctl & TIMER_CTL_KEEP) | TIMER_CTL_TICK_EN | TIMER_CTL_WDOG_EN,
		  base + REG_TIMER_CTL);

	timer_setup(&kick_timer, en75_wdt_kick_timer, 0);
	mod_timer(&kick_timer, jiffies + msecs_to_jiffies(kick_ms));

	pr_info("en75-wdt: armed, %us timeout, kicked every %ums (ctl %08x -> %08x)\n",
		timeout, kick_ms, ctl, ioread32(base + REG_TIMER_CTL));
	return 0;
}

static void __exit en75_wdt_exit(void)
{
	u32 ctl;

	timer_delete_sync(&kick_timer);

	ctl = ioread32(base + REG_TIMER_CTL);
	iowrite32(ctl & ~(TIMER_CTL_WDOG_EN | TIMER_CTL_TICK_EN), base + REG_TIMER_CTL);
	en75_wdt_kick();

	iounmap(base);
	pr_info("en75-wdt: disarmed\n");
}

module_init(en75_wdt_init);
module_exit(en75_wdt_exit);

MODULE_DESCRIPTION("EN751221 watchdog for lockup capture");
MODULE_LICENSE("GPL");
