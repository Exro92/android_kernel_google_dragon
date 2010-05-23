/*
 * arch/arm/mach-tegra/gpio.c
 *
 * The tegra gpio driver.
 *
 * Copyright (c) 2010 Google, Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include "nvrm_pmu.h"
#include "nvos.h"
#include "nvodm_query_discovery.h"

#include <linux/init.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/interrupt.h>

#include <mach/nvrm_linux.h>
#include "nvrm_pinmux_utils.h"

#include <asm/io.h>
#include <asm/gpio.h>

#include <mach/pinmux.h>

#define GPIO_BANK(x)        ((x) >> 5)
#define GPIO_PORT(x)        (((x) >> 3) & 0x3)
#define GPIO_BIT(x)         ((x) & 0x7)

extern int gpio_get_pinmux_group(int gpio_nr);
extern unsigned long tegra_get_module_inst_base(const char *name, int inst);
static unsigned long add_gpio_base = 0;
#define GPIO_REG(x)   ((add_gpio_base + GPIO_BANK(x)*0x80) +  GPIO_PORT(x)*4)

#define GPIO_CNF(x)     (GPIO_REG(x) + 0x00)
#define GPIO_OE(x)      (GPIO_REG(x) + 0x10)
#define GPIO_OUT(x)     (GPIO_REG(x) + 0X20)
#define GPIO_IN(x)      (GPIO_REG(x) + 0x30)

#define GPIO_INT_STA(x)     (GPIO_REG(x) + 0x40)
#define GPIO_INT_ENB(x)     (GPIO_REG(x) + 0x50)
#define GPIO_INT_LVL(x)     (GPIO_REG(x) + 0x60)
#define GPIO_INT_CLR(x)     (GPIO_REG(x) + 0x70)

#define GPIO_MSK_CNF(x)     (GPIO_REG(x) + 0x800)
#define GPIO_MSK_OE(x)      (GPIO_REG(x) + 0x810)
#define GPIO_MSK_OUT(x)     (GPIO_REG(x) + 0X820)
#define GPIO_MSK_INT_STA(x) (GPIO_REG(x) + 0x840)
#define GPIO_MSK_INT_ENB(x) (GPIO_REG(x) + 0x850)
#define GPIO_MSK_INT_LVL(x) (GPIO_REG(x) + 0x860)

#define GPIO_INT_LVL_MASK           0x010101
#define GPIO_INT_LVL_EDGE_RISING    0x000101
#define GPIO_INT_LVL_EDGE_FALLING   0x000100
#define GPIO_INT_LVL_EDGE_BOTH      0x010100
#define GPIO_INT_LVL_LEVEL_HIGH     0x000001
#define GPIO_INT_LVL_LEVEL_LOW      0x000000

#define MAX_GPIO_INSTANCES  10

int tegra_gpio_io_power_config(int gpio_nr, unsigned int enable);

struct tegra_gpio_bank {
	int bank;
	int irq;
	spinlock_t lvl_lock[4];
#ifdef CONFIG_PM
	u32 cnf[4];
	u32 out[4];
	u32 oe[4];
	u32 int_enb[4];
	u32 int_lvl[4];
#endif
};

static struct tegra_gpio_bank tegra_gpio_banks[] = {
	{.bank = 0, .irq = INT_GPIO1},
	{.bank = 1, .irq = INT_GPIO2},
	{.bank = 2, .irq = INT_GPIO3},
	{.bank = 3, .irq = INT_GPIO4},
	{.bank = 4, .irq = INT_GPIO5},
	{.bank = 5, .irq = INT_GPIO6},
	{.bank = 6, .irq = INT_GPIO7},
};

static int tegra_gpio_compose(int bank, int port, int bit)
{
	return (bank << 5) | ((port & 0x3) << 3) | (bit & 0x7);
}

static void tegra_gpio_mask_write(u32 reg, int gpio, int value)
{
	u32 val;

	val = 0x100 << GPIO_BIT(gpio);
	if (value)
		val |= 1 << GPIO_BIT(gpio);
	__raw_writel(val, reg);
}

void tegra_gpio_enable(int gpio)
{
	tegra_gpio_mask_write(GPIO_MSK_CNF(gpio), gpio, 1);
}

void tegra_gpio_disable(int gpio)
{
	tegra_gpio_mask_write(GPIO_MSK_CNF(gpio), gpio, 0);
}

static void tegra_set_gpio_tristate(int gpio_nr, tegra_tristate_t ts)
{
	tegra_pingroup_t pg;
	int err;

	if (gpio_nr >= TEGRA_MAX_GPIO)
		return;
	pg = gpio_get_pinmux_group(gpio_nr);
	if (pg >= 0) {
		err = tegra_pinmux_set_tristate(pg, ts);
		if (err < 0)
			printk(KERN_ERR "pinmux: can't set pingroup %d tristate"
					" to %d: %d\n", pg, ts, err);
	}
}

static int tegra_gpio_request(struct gpio_chip *chip, unsigned offset)
{
	int port;
	int pin;
	int status;
	port = GPIO_BANK(offset) * 4 + GPIO_PORT(offset);
	pin = GPIO_BIT(offset);

	status = tegra_gpio_io_power_config(offset, true);
	if (unlikely(status != 0)) {
		return -1;
	}

	tegra_gpio_mask_write(GPIO_MSK_CNF(offset), offset, 1);
	tegra_set_gpio_tristate(offset, TEGRA_TRI_NORMAL);
	return 0;
}

static void tegra_gpio_free(struct gpio_chip *chip, unsigned offset)
{
	int port;
	int pin;
	port = GPIO_BANK(offset) * 4 + GPIO_PORT(offset);
	pin = GPIO_BIT(offset);

	tegra_gpio_io_power_config(offset, false);
	tegra_gpio_mask_write(GPIO_MSK_CNF(offset), offset, 0);
	tegra_set_gpio_tristate(offset, TEGRA_TRI_TRISTATE);
}

static void tegra_gpio_set(struct gpio_chip *chip, unsigned offset, int value)
{
	tegra_gpio_mask_write(GPIO_MSK_OUT(offset), offset, value);
}

static int tegra_gpio_get(struct gpio_chip *chip, unsigned offset)
{
	return (__raw_readl(GPIO_IN(offset)) >> GPIO_BIT(offset)) & 0x1;
}

static int tegra_gpio_direction_input(struct gpio_chip *chip, unsigned offset)
{
	tegra_gpio_mask_write(GPIO_MSK_OE(offset), offset, 0);
	return 0;
}

static int tegra_gpio_direction_output(struct gpio_chip *chip,
	unsigned offset, int value)
{
	tegra_gpio_mask_write(GPIO_MSK_OE(offset), offset, 1);
	return 0;
}

static struct gpio_chip tegra_gpio_chip = {
	.label              = "tegra-gpio",
	.request            = tegra_gpio_request,
	.free               = tegra_gpio_free,
	.direction_input    = tegra_gpio_direction_input,
	.get                = tegra_gpio_get,
	.direction_output   = tegra_gpio_direction_output,
	.set                = tegra_gpio_set,
	.base               = 0,
	.ngpio              = ARCH_NR_GPIOS,
};

static void tegra_gpio_irq_ack(unsigned int irq)
{
	int gpio = irq - INT_GPIO_BASE;

	__raw_writel(1 << GPIO_BIT(gpio), GPIO_INT_CLR(gpio));
}

static void tegra_gpio_irq_mask(unsigned int irq)
{
	int gpio = irq - INT_GPIO_BASE;

	tegra_gpio_mask_write(GPIO_MSK_INT_ENB(gpio), gpio, 0);
}

static void tegra_gpio_irq_unmask(unsigned int irq)
{
	int gpio = irq - INT_GPIO_BASE;

	tegra_gpio_mask_write(GPIO_MSK_INT_ENB(gpio), gpio, 1);
}

static int tegra_gpio_irq_set_type(unsigned int irq, unsigned int type)
{
	int gpio = irq - INT_GPIO_BASE;
	struct tegra_gpio_bank *bank = get_irq_chip_data(irq);
	int port = GPIO_PORT(gpio);
	int lvl_type;
	int val;
	unsigned long flags;

	switch (type) {
	case IRQ_TYPE_EDGE_RISING:
		lvl_type = GPIO_INT_LVL_EDGE_RISING;
		break;

	case IRQ_TYPE_EDGE_FALLING:
		lvl_type = GPIO_INT_LVL_EDGE_FALLING;
		break;

	case IRQ_TYPE_EDGE_BOTH:
		lvl_type = GPIO_INT_LVL_EDGE_BOTH;
		break;

	case IRQ_TYPE_LEVEL_HIGH:
		lvl_type = GPIO_INT_LVL_LEVEL_HIGH;
		break;

	case IRQ_TYPE_LEVEL_LOW:
		lvl_type = GPIO_INT_LVL_LEVEL_LOW;
		break;

	default:
		return -EINVAL;
	}

	spin_lock_irqsave(&bank->lvl_lock[port], flags);

	val = __raw_readl(GPIO_INT_LVL(gpio));
	val &= ~(GPIO_INT_LVL_MASK << GPIO_BIT(gpio));
	val |= lvl_type << GPIO_BIT(gpio);
	__raw_writel( val, GPIO_INT_LVL(gpio));

	spin_unlock_irqrestore(&bank->lvl_lock[port], flags);

	if (type & (IRQ_TYPE_LEVEL_LOW | IRQ_TYPE_LEVEL_HIGH))
		__set_irq_handler_unlocked(irq, handle_level_irq);
	else if (type & (IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_EDGE_RISING))
		__set_irq_handler_unlocked(irq, handle_edge_irq);

	return 0;
}

#ifdef CONFIG_PM
void tegra_gpio_resume(void)
{
	unsigned long flags;
	int b, p, i;

	local_irq_save(flags);

	for (b=0; b<ARRAY_SIZE(tegra_gpio_banks); b++) {
		struct tegra_gpio_bank *bank = &tegra_gpio_banks[b];

		for (p=0; p<ARRAY_SIZE(bank->oe); p++) {
			unsigned int gpio = (b<<5) | (p<<3);
			__raw_writel(bank->cnf[p], GPIO_CNF(gpio));
			__raw_writel(bank->out[p], GPIO_OUT(gpio));
			__raw_writel(bank->oe[p], GPIO_OE(gpio));
			__raw_writel(bank->int_lvl[p], GPIO_INT_LVL(gpio));
			__raw_writel(bank->int_enb[p], GPIO_INT_ENB(gpio));
		}

	}

	local_irq_restore(flags);

	for (i=INT_GPIO_BASE; i<(INT_GPIO_BASE+ARCH_NR_GPIOS); i++) {
		struct irq_desc *desc = irq_to_desc(i);
		if (!desc || (desc->status & IRQ_WAKEUP)) continue;
		enable_irq(i);
	}
}

void tegra_gpio_suspend(void)
{
	unsigned long flags;
	int b, p, i;


	for (i=INT_GPIO_BASE; i<(INT_GPIO_BASE+ARCH_NR_GPIOS); i++) {
		struct irq_desc *desc = irq_to_desc(i);
		if (!desc) continue;
		if (desc->status & IRQ_WAKEUP) {
			int gpio = i - INT_GPIO_BASE;
			pr_debug("gpio %d.%d is wakeup\n", gpio/8, gpio&7);
			continue;
                }
		disable_irq(i);
	}

	local_irq_save(flags);
	for (b=0; b<ARRAY_SIZE(tegra_gpio_banks); b++) {
		struct tegra_gpio_bank *bank = &tegra_gpio_banks[b];

		for (p=0; p<ARRAY_SIZE(bank->oe); p++) {
			unsigned int gpio = (b<<5) | (p<<3);
			bank->cnf[p] = __raw_readl(GPIO_CNF(gpio));
			bank->out[p] = __raw_readl(GPIO_OUT(gpio));
			bank->oe[p] = __raw_readl(GPIO_OE(gpio));
			bank->int_enb[p] = __raw_readl(GPIO_INT_ENB(gpio));
			bank->int_lvl[p] = __raw_readl(GPIO_INT_LVL(gpio));
		}

	}
	local_irq_restore(flags);
}

static int tegra_gpio_wake_enable(unsigned int irq, unsigned int enable)
{
	struct tegra_gpio_bank *bank = get_irq_chip_data(irq);
	return set_irq_wake(bank->irq, enable);
}
#endif

static struct irq_chip tegra_gpio_irq_chip = {
	.name	   = "GPIO",
	.ack        = tegra_gpio_irq_ack,
	.mask       = tegra_gpio_irq_mask,
	.unmask     = tegra_gpio_irq_unmask,
	.set_type   = tegra_gpio_irq_set_type,
#ifdef CONFIG_PM
	.set_wake   = tegra_gpio_wake_enable,
#endif
};

static void tegra_gpio_irq_handler(unsigned int irq, struct irq_desc *desc)
{
	struct tegra_gpio_bank *bank;
	int port;
	int pin;
	int unmasked = 0;

	desc->chip->ack(irq);

	bank = get_irq_data(irq);

	for (port = 0; port < 4; port++) {
		int gpio = tegra_gpio_compose(bank->bank, port, 0);
		u8 sta = __raw_readl(GPIO_INT_STA(gpio)) &
			__raw_readl(GPIO_INT_ENB(gpio));
		u32 lvl = __raw_readl(GPIO_INT_LVL(gpio));

		for (pin = 0; pin < 8; pin++) {
			if (sta & (1 << pin)) {
				__raw_writel(1 << pin,
						 GPIO_INT_CLR(gpio));

				/* if gpio is edge triggered, clear condition
				 * before executing the hander so that we don't
				 * miss edges
				 */
				if (lvl & (0x100 << pin)) {
					unmasked = 1;
					desc->chip->unmask(irq);
				}

				generic_handle_irq(gpio_to_irq(gpio + pin));
			}
		}
	}
	if (!unmasked)
		desc->chip->unmask(irq);

}

/* This lock class tells lockdep that GPIO irqs are in a different
 * category than their parents, so it won't report false recursion.
 */
static struct lock_class_key gpio_lock_class;

static int __init tegra_gpio_init(void)
{
	struct tegra_gpio_bank *bank;
	int i;
	int j;
	unsigned long phys;

	phys = tegra_get_module_inst_base("gpio",0);
	add_gpio_base = (unsigned long)IO_ADDRESS(phys);

	for (i = 0; i < ARRAY_SIZE(tegra_gpio_banks); i++) {
		for (j = 0; j < 4; j++) {
			int gpio = tegra_gpio_compose(i, j, 0);
			__raw_writel(0x00, GPIO_INT_ENB(gpio));
		}
	}

	gpiochip_add(&tegra_gpio_chip);

	for (i = INT_GPIO_BASE; i < (INT_GPIO_BASE + ARCH_NR_GPIOS); i++) {
		bank = &tegra_gpio_banks[GPIO_BANK(i-INT_GPIO_BASE)];

		lockdep_set_class(&irq_desc[i].lock, &gpio_lock_class);
		set_irq_chip_data(i, bank);
		set_irq_chip(i, &tegra_gpio_irq_chip);
		set_irq_handler(i, handle_level_irq);
		set_irq_flags(i, IRQF_VALID);
	}

	for (i = 0; i < ARRAY_SIZE(tegra_gpio_banks); i++) {
		bank = &tegra_gpio_banks[i];

		set_irq_chained_handler(bank->irq, tegra_gpio_irq_handler);
		set_irq_data(bank->irq, bank);

		for (j = 0; j < 4; j++)
			bank->lvl_lock[j] = SPIN_LOCK_UNLOCKED;
	}
	return 0;
}

postcore_initcall(tegra_gpio_init);

#ifdef  CONFIG_DEBUG_FS

#include <linux/debugfs.h>
#include <linux/seq_file.h>

static int dbg_gpio_show(struct seq_file *s, void *unused)
{
	int i;
	int j;

	for (i = 0; i < 7; i++) {
		for (j = 0; j < 4; j++) {
			int gpio = tegra_gpio_compose(i, j, 0);
			seq_printf(s, "%d:%d %02x %02x %02x %02x %02x %02x %06x\n",
				   i, j,
				   __raw_readl(GPIO_CNF(gpio)),
				   __raw_readl(GPIO_OE(gpio)),
				   __raw_readl(GPIO_OUT(gpio)),
				   __raw_readl(GPIO_IN(gpio)),
				   __raw_readl(GPIO_INT_STA(gpio)),
				   __raw_readl(GPIO_INT_ENB(gpio)),
				   __raw_readl(GPIO_INT_LVL(gpio)));
		}
	}
	return 0;
}

static int dbg_gpio_open(struct inode *inode, struct file *file)
{
	return single_open(file, dbg_gpio_show, &inode->i_private);
}

static const struct file_operations debug_fops = {
	.open       = dbg_gpio_open,
	.read       = seq_read,
	.llseek     = seq_lseek,
	.release    = single_release,
};
static int __init tegra_gpio_debuginit(void)
{
	(void) debugfs_create_file("tegra_gpio", S_IRUGO,
					NULL, NULL, &debug_fops);
	return 0;
}
late_initcall(tegra_gpio_debuginit);
#endif

struct gpio_power_rail_info {
	/* SoC power rail GUID */
	NvU64 power_rail_guid;

	/* Pmu rail address */
	NvU32 power_rail_address;
};

static unsigned int is_gpio_rail_initailized  =  0;
static struct gpio_power_rail_info gpio_power_rail_table[] = {
	{.power_rail_guid = NV_VDD_SYS_ODM_ID,  .power_rail_address = 0},
	{.power_rail_guid = NV_VDD_BB_ODM_ID,   .power_rail_address = 0},
	{.power_rail_guid = NV_VDD_VI_ODM_ID,   .power_rail_address = 0},
	{.power_rail_guid = NV_VDD_SDIO_ODM_ID, .power_rail_address = 0},
	{.power_rail_guid = NV_VDD_LCD_ODM_ID,  .power_rail_address = 0},
	{.power_rail_guid = NV_VDD_UART_ODM_ID, .power_rail_address = 0},
};

/* Initialize power rails for different gpios pins */
static struct gpio_power_rail_info *gpio_power_rail_map[ARCH_NR_GPIOS] = {
	/* Port a */
	&gpio_power_rail_table[3],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[3],
	&gpio_power_rail_table[3],

	/* Port b */
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[3],
	&gpio_power_rail_table[3],
	&gpio_power_rail_table[3],
	&gpio_power_rail_table[3],

	/* Port c */
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[5],

	/* Port d */
	&gpio_power_rail_table[3],
	&gpio_power_rail_table[3],
	&gpio_power_rail_table[3],
	&gpio_power_rail_table[3],
	&gpio_power_rail_table[3],
	&gpio_power_rail_table[2],
	&gpio_power_rail_table[2],
	&gpio_power_rail_table[2],

	/* Port e */
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],

	/* Port f */
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],

	/* Port g */
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],

	/* Port h */
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],

	/* Port i */
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],

	/* Port j */
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],

	/* Port k */
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],

	/* Port l */
	&gpio_power_rail_table[2],
	&gpio_power_rail_table[2],
	&gpio_power_rail_table[2],
	&gpio_power_rail_table[2],
	&gpio_power_rail_table[2],
	&gpio_power_rail_table[2],
	&gpio_power_rail_table[2],
	&gpio_power_rail_table[2],

	/* Port m */
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],

	/* Port n */
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],

	/* Port o */
	&gpio_power_rail_table[1],
	&gpio_power_rail_table[1],
	&gpio_power_rail_table[1],
	&gpio_power_rail_table[1],
	&gpio_power_rail_table[1],
	&gpio_power_rail_table[1],
	&gpio_power_rail_table[1],
	&gpio_power_rail_table[1],

	/* Port p */
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],

	/* Port q */
	&gpio_power_rail_table[0],
	&gpio_power_rail_table[0],
	&gpio_power_rail_table[0],
	&gpio_power_rail_table[0],
	&gpio_power_rail_table[0],
	&gpio_power_rail_table[0],
	&gpio_power_rail_table[0],
	&gpio_power_rail_table[0],

	/* Port r */
	&gpio_power_rail_table[0],
	&gpio_power_rail_table[0],
	&gpio_power_rail_table[0],
	&gpio_power_rail_table[0],
	&gpio_power_rail_table[0],
	&gpio_power_rail_table[0],
	&gpio_power_rail_table[0],
	&gpio_power_rail_table[0],

	/* Port s */
	&gpio_power_rail_table[0],
	&gpio_power_rail_table[0],
	&gpio_power_rail_table[0],
	&gpio_power_rail_table[0],
	&gpio_power_rail_table[0],
	&gpio_power_rail_table[0],
	&gpio_power_rail_table[0],
	&gpio_power_rail_table[0],

	/* Port t */
	&gpio_power_rail_table[2],
	&gpio_power_rail_table[2],
	&gpio_power_rail_table[2],
	&gpio_power_rail_table[2],
	&gpio_power_rail_table[2],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],

	/* Port u */
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],

	/* Port v */
	&gpio_power_rail_table[1],
	&gpio_power_rail_table[1],
	&gpio_power_rail_table[1],
	&gpio_power_rail_table[1],
	&gpio_power_rail_table[3],
	&gpio_power_rail_table[3],
	&gpio_power_rail_table[3],
	&gpio_power_rail_table[4],

	/* Port w */
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[4],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],

	/* Port x */
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],

	/* Port y */
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],

	/* Port z */
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],

	/* Port AA */
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],

	/* Port BB */
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5],
	&gpio_power_rail_table[5]
};

static void discover_gpio_io_power_rail(void)
{
	unsigned int i;
	const NvOdmPeripheralConnectivity* connectivity = NULL;

	for (i = 0; i < NV_ARRAY_SIZE(gpio_power_rail_table); i++) {
		connectivity = NvOdmPeripheralGetGuid(
				gpio_power_rail_table[i].power_rail_guid);
		if (!connectivity || !connectivity->NumAddress)
			continue;
		gpio_power_rail_table[i].power_rail_address =
				connectivity->AddressList[0].Address;
	}
}
int tegra_gpio_io_power_config(int gpio_nr, unsigned int enable)
{
	NvRmPmuVddRailCapabilities rail_caps;
	NvU32 settling_time;
	struct gpio_power_rail_info *gpio_io_power;

	if (!is_gpio_rail_initailized) {
		discover_gpio_io_power_rail();
		is_gpio_rail_initailized = 1;
	}

	gpio_io_power = gpio_power_rail_map[gpio_nr];

	/* Nothing to be done if there is no pmu rail
	 * associated with this port */
	if (gpio_io_power->power_rail_address == 0)
		return 0;

	if (enable) {
		NvRmPmuGetCapabilities(s_hRmGlobal, gpio_io_power->power_rail_address,
					&rail_caps);
		NvRmPmuSetVoltage(s_hRmGlobal, gpio_io_power->power_rail_address,
					rail_caps.requestMilliVolts,
					&settling_time);
	} else {
		NvRmPmuSetVoltage(s_hRmGlobal, gpio_io_power->power_rail_address,
					ODM_VOLTAGE_OFF, &settling_time);
	}
	if (settling_time)
		NvOsWaitUS(settling_time);

	return 0;
}
