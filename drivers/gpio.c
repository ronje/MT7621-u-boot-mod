/******************************************************************************
* Filename : gpio.c
* This part is used to control LED and detect button-press
* (从 mt7628_uboot_web 移植, 按 AX1800(MT7621) IO 方案定制:
*   reset=GPIO16, hdog=GPIO14, usbpower=GPIO13(低=上电), sysLed=GPIO17)
******************************************************************************/

#include <common.h>
#include "../autoconf.h"
#include <configs/rt2880.h>
#include <rt_mmap.h>
#include <gpio.h>

#define ARRAY_SIZE(x)		(sizeof(x) / sizeof((x)[0]))

#define ra_inl(offset)		(*(volatile unsigned long *)(offset))
#define ra_outl(offset,val)	(*(volatile unsigned long *)(offset) = val)
#define ra_and(addr, value)	ra_outl(addr, (ra_inl(addr) & (value)))
#define ra_or(addr, value)	ra_outl(addr, (ra_inl(addr) | (value)))

#define GPIO_DIR_INPUT		0
#define GPIO_DIR_OUTPUT		1

#if defined(MT7621_MP)

#if defined(BOARD_AX1800)
/* AX1800 (MT7621) GPIO 映射(全部落在 JTAG 复用组 GPIO13~17):
 *   GPIO_BTN_RESET  16  复位键, 低有效, 长按分级入口(3~8s=httpd / >8s=TFTP)
 *   GPIO_HW_WDT     14  硬件看门狗喂狗端, 2 分钟内需一个 ≥50ms 高电平脉冲
 *   GPIO_USBPOWER   13  USB 口供电, 低=上电(on 0), uboot 阶段保持供电
 *   RUN_LED=17      sysLed, 默认低=亮(GPIO_LED_INVERTED 可整体翻转)
 *   wwan1Led=15 / wwan2Led=18: 由 Linux 驱动, uboot 不占用(-1)
 * 注: GPIO18 在 WDT 复用组; gpio_init 的 gm 统一把 JTAG/WDT 组切 GPIO 模式。 */
#define GPIO_BTN_RESET	16
#define GPIO_USBPOWER	13
#define GPIO_USBPOWER_ON_LEVEL	0	/* 低=上电 */
#define GPIO_HW_WDT	14
#else
/* FireWRT 原板: WPS 键 GPIO18 兼作 reset, 无看门狗/USB 供电脚 */
#define GPIO_BTN_RESET 18
#define GPIO_USBPOWER	-1
#define GPIO_HW_WDT	-1
#endif

#ifndef GPIO_HW_WDT
#define GPIO_HW_WDT		-1	/* 未定义 = 无外置看门狗, kick 为空操作 */
#endif

/* 单板型号标识(启动时打印, 便于产线核对固件与单板匹配) */
#if defined(BOARD_AX1800)
#define BOARD_NAME_STR		"ax1800"
#elif defined(MT7621_MP)
#define BOARD_NAME_STR		"MT7621"
#endif

#elif defined(MT7628_MP)

#define GPIO_BTN_RESET 11
#define GPIO_HW_WDT		-1

#elif defined(MT7620_MP)

#define GPIO_BTN_RESET 13
#define GPIO_HW_WDT		-1

#else

#define GPIO_BTN_RESET	-1
#define GPIO_HW_WDT		-1

#endif

#if defined (GPIO_LED_INVERTED)
#define GPIO_VAL_LED_SHOW	1
#define GPIO_VAL_LED_HIDE	0
#else
#define GPIO_VAL_LED_SHOW	0
#define GPIO_VAL_LED_HIDE	1
#endif

#define GPIO_VAL_BTN_PRESSED	0

#if defined(RT3052_MP2)

//RT3052
#define RALINK_GPIOMODE_I2C		(1U << 0)
#define RALINK_GPIOMODE_SPI		(1U << 1)
#define RALINK_GPIOMODE_UARTF		(7U << 2)
#define RALINK_GPIOMODE_UARTL		(1U << 5)
#define RALINK_GPIOMODE_JTAG		(1U << 6)
#define RALINK_GPIOMODE_MDIO		(1U << 7)
#define RALINK_GPIOMODE_SDRAM		(1U << 8)
#define RALINK_GPIOMODE_RGMII		(1U << 9)

#elif defined(RT3352_MP)

//RT3352
#define RALINK_GPIOMODE_I2C		(1U << 0)
#define RALINK_GPIOMODE_SPI		(1U << 1)
#define RALINK_GPIOMODE_UARTF		(7U << 2)
#define RALINK_GPIOMODE_UARTL		(1U << 5)
#define RALINK_GPIOMODE_JTAG		(1U << 6)
#define RALINK_GPIOMODE_MDIO		(1U << 7)
#define RALINK_GPIOMODE_GE1		(1U << 9)
#define RALINK_GPIOMODE_EPHY_BT		(1U << 14)
#define RALINK_GPIOMODE_LNA_G		(1U << 18)
#define RALINK_GPIOMODE_PA_G		(1U << 20)
#define RALINK_GPIOMODE_SPI_CS1		(1U << 22)

#elif defined(RT5350_MP)

//RT5350
#define RALINK_GPIOMODE_I2C		(1U << 0)
#define RALINK_GPIOMODE_SPI		(1U << 1)
#define RALINK_GPIOMODE_UARTF		(7U << 2)
#define RALINK_GPIOMODE_UARTL		(1U << 5)
#define RALINK_GPIOMODE_JTAG		(1U << 6)
#define RALINK_GPIOMODE_EPHY_BT		(1U << 14)
#define RALINK_GPIOMODE_SPI_CS1		(1U << 22)

#elif defined(RT3883_MP)

//RT3883
#define RALINK_GPIOMODE_I2C		(1U << 0)	/* GPIO #1~#2 */
#define RALINK_GPIOMODE_SPI		(1U << 1)	/* GPIO #3~#6 */
#define RALINK_GPIOMODE_UARTF		(7U << 2)	/* GPIO #7~#14 */
#define RALINK_GPIOMODE_UARTL		(1U << 5)	/* GPIO #15~#16 */
#define RALINK_GPIOMODE_JTAG		(1U << 6)	/* GPIO #17~#21 */
#define RALINK_GPIOMODE_MDIO		(1U << 7)	/* GPIO #22~#23 */
#define RALINK_GPIOMODE_GE1		(1U << 9)	/* GPIO #84~#95 */
#define RALINK_GPIOMODE_GE2		(1U << 10)	/* GPIO #72~#83 */
#define RALINK_GPIOMODE_PCI		(3U << 11)	/* GPIO #40~#71 */
#define RALINK_GPIOMODE_LNA_A		(3U << 16)	/* GPIO #32~#34 */
#define RALINK_GPIOMODE_LNA_G		(3U << 18)	/* GPIO #35~#37 */

#elif defined(MT7620_MP)

//MT7620
#define RALINK_GPIOMODE_I2C		(1U << 0)	/* GPIO #1~#2 */
#define RALINK_GPIOMODE_UARTF		(7U << 2)	/* GPIO #7~#14 */
#define RALINK_GPIOMODE_UARTL		(1U << 5)	/* GPIO#15~#16 */
#define RALINK_GPIOMODE_MDIO		(3U << 7)	/* GPIO#22~#23 */
#define RALINK_GPIOMODE_RGMII1		(1U << 9)	/* GPIO#24~#35 */
#define RALINK_GPIOMODE_RGMII2		(1U << 10)	/* GPIO#60~#71 */
#define RALINK_GPIOMODE_SPI		(1U << 11)	/* GPIO#3~#6 */
#define RALINK_GPIOMODE_SPI_REFCLK	(1U << 12)	/* GPIO#37 */
#define RALINK_GPIOMODE_WLED		(1U << 13)	/* GPIO#72 */
#define RALINK_GPIOMODE_JTAG		(1U << 15)	/* GPIO#40~#44 */
#define RALINK_GPIOMODE_PERST		(3U << 16)	/* GPIO#36 */
#define RALINK_GPIOMODE_NAND_SD		(3U << 18)	/* GPIO#45~#59 */
#define RALINK_GPIOMODE_PA		(1U << 20)	/* GPIO#18~#21 */
#define RALINK_GPIOMODE_WDT		(3U << 21)	/* GPIO#17 */

#elif defined(MT7621_MP)

//MT7621
#define RALINK_GPIOMODE_UART1		(1U << 1)	/* GPIO #1~#2 */
#define RALINK_GPIOMODE_I2C		(1U << 2)	/* GPIO #3~#4 */
#define RALINK_GPIOMODE_UART3		(1U << 3)	/* GPIO #5~#8 */
#define RALINK_GPIOMODE_UART2		(1U << 5)	/* GPIO #9~#12 */
#define RALINK_GPIOMODE_JTAG		(1U << 7)	/* GPIO #13~#17 */
#define RALINK_GPIOMODE_WDT		(1U << 8)	/* GPIO #18 */
#define RALINK_GPIOMODE_PERST		(1U << 10)	/* GPIO #19 */
#define RALINK_GPIOMODE_MDIO		(3U << 12)	/* GPIO #20~#21 */
#define RALINK_GPIOMODE_RGMII1		(1U << 14)	/* GPIO #49~#60 */
#define RALINK_GPIOMODE_RGMII2		(1U << 15)	/* GPIO #22~#33 */
#define RALINK_GPIOMODE_SPI		(1U << 16)	/* GPIO #34~#40 */
#define RALINK_GPIOMODE_SDXC		(1U << 18)	/* GPIO #41~#48 */
#define RALINK_GPIOMODE_ESWINT		(1U << 20)	/* GPIO #61 */

#elif defined(MT7628_MP)

//MT7628
#define RALINK_GPIOMODE_GPIO		(1U << 0)	/* GPIO #11 */
#define RALINK_GPIOMODE_SPI_SLAVE	(1U << 2)	/* GPIO #14~#17 */
#define RALINK_GPIOMODE_SPI_CS1		(1U << 4)	/* GPIO #6 */
#define RALINK_GPIOMODE_I2S		(1U << 6)	/* GPIO #0~#3 */
#define RALINK_GPIOMODE_UART1		(1U << 8)	/* GPIO #12~#13 */
#define RALINK_GPIOMODE_SDXC		(1U << 10)	/* GPIO #22~#29 */
#define RALINK_GPIOMODE_SPI		(1U << 12)	/* GPIO #7~#10 */
#define RALINK_GPIOMODE_WDT		(1U << 14)	/* GPIO #38 */
#define RALINK_GPIOMODE_PERST		(1U << 16)	/* GPIO #36 */
#define RALINK_GPIOMODE_REFCLK		(1U << 18)	/* GPIO #37 */
#define RALINK_GPIOMODE_I2C		(1U << 20)	/* GPIO #4~#5 */
#define RALINK_GPIOMODE_UART2		(1U << 24)	/* GPIO #45~#46 */
#define RALINK_GPIOMODE_UART3		(1U << 26)	/* GPIO #20~#21 */
#define RALINK_GPIOMODE_PWM0		(1U << 28)	/* GPIO #18 */
#define RALINK_GPIOMODE_PWM1		(1U << 30)	/* GPIO #19 */

#endif

enum gpio_reg_id {
	GPIO_INT = 0,
	GPIO_EDGE,
	GPIO_RMASK,
	GPIO_MASK,
	GPIO_DATA,
	GPIO_DIR,
	GPIO_POL,
	GPIO_SET,
	GPIO_RESET,
	GPIO_MAX_REG
};

const static struct gpio_reg_offset_s {
	unsigned short min_nr, max_nr;
	unsigned short int_offset;
	unsigned short edge_offset;
	unsigned short rmask_offset;
	unsigned short fmask_offset;
	unsigned short data_offset;
	unsigned short dir_offset;
	unsigned short pol_offset;
	unsigned short set_offset;
	unsigned short reset_offset;
} s_gpio_reg_offset[] = {
#if defined(RT3052_MP2) || defined(RT3352_MP) || defined(RT3883_MP) || defined(MT7620_MP)
	{  0, 23, 0x00, 0x04, 0x08, 0x0c, 0x20, 0x24, 0x28, 0x2c, 0x30 },
	{ 24, 39, 0x38, 0x3c, 0x40, 0x44, 0x48, 0x4c, 0x50, 0x54, 0x58 },
	{ 40, 71, 0x60, 0x64, 0x68, 0x6c, 0x70, 0x74, 0x78, 0x7c, 0x80 },
	{ 72, 95, 0x88, 0x8c, 0x90, 0x94, 0x98, 0x9c, 0xa0, 0xa4, 0xa8 },
#elif defined(RT5350_MP)
	{  0, 21, 0x00, 0x04, 0x08, 0x0c, 0x20, 0x24, 0x28, 0x2c, 0x30 },
	{ 22, 27, 0x60, 0x64, 0x68, 0x6c, 0x70, 0x74, 0x78, 0x7c, 0x80 },
#elif defined(MT7621_MP) || defined(MT7628_MP)
	{  0, 31, 0x90, 0xA0, 0x50, 0x60, 0x20, 0x00, 0x10, 0x30, 0x40 },
	{ 32, 63, 0x94, 0xA4, 0x54, 0x64, 0x24, 0x04, 0x14, 0x34, 0x44 },
	{ 64, 95, 0x98, 0xA8, 0x58, 0x68, 0x28, 0x08, 0x18, 0x38, 0x48 },
#endif
};

static inline int is_valid_gpio_nr(unsigned short gpio_nr)
{
#if defined(RT3052_MP2)
	return (gpio_nr > 51)? 0:1;
#elif defined(RT3352_MP)
	return (gpio_nr > 45)? 0:1;
#elif defined(RT5350_MP)
	return (gpio_nr > 27)? 0:1;
#elif defined(RT3883_MP)
	return (gpio_nr > 95)? 0:1;
#elif defined(MT7620_MP)
	return (gpio_nr > 72)? 0:1;
#elif defined(MT7621_MP)
	return (gpio_nr > 61)? 0:1;
#elif defined(MT7628_MP)
	return (gpio_nr > 46)? 0:1;
#else
	return 0;
#endif
}

/* Query GPIO number belongs which item.
 * @gpio_nr:	GPIO number
 * @return:
 *	NULL:	Invalid parameter.
 *  otherwise:	Pointer to a gpio_reg_offset_s instance.
 */
static const struct gpio_reg_offset_s *get_gpio_reg_item(unsigned short gpio_nr)
{
	int i;
	const struct gpio_reg_offset_s *p = &s_gpio_reg_offset[0], *ret = NULL;

	if (!is_valid_gpio_nr(gpio_nr))
		return ret;

	for (i = 0; !ret && i < ARRAY_SIZE(s_gpio_reg_offset); ++i, ++p) {
		if (gpio_nr < p->min_nr || gpio_nr > p->max_nr)
			continue;
		ret = p;
	}

	return ret;
}

/* Return bit-shift of a GPIO.
 * @gpio_nr:	GPIO number
 * @return:
 * 	0~31:	bit-shift of a GPIO pin in a register.
 *  	-1:	Invalid parameter.
 */
static int get_gpio_reg_bit_shift(unsigned short gpio_nr)
{
	const struct gpio_reg_offset_s *p;

	if (!(p = get_gpio_reg_item(gpio_nr)))
		return -1;

	return gpio_nr - p->min_nr;
}

/* Return specific GPIO register in accordance with GPIO number
 * @gpio_nr:	GPIO number
 * @return
 * 	0:	invalid parameter
 *  otherwise:	address of GPIO register
 */

static unsigned int get_gpio_reg_addr(unsigned short gpio_nr, enum gpio_reg_id id)
{
	unsigned int ret = 0;
	const struct gpio_reg_offset_s *p;

	if (!(p = get_gpio_reg_item(gpio_nr)) || id < 0 || id >= GPIO_MAX_REG)
		return ret;

	switch (id) {
	case GPIO_INT:
		ret = p->int_offset;
		break;
	case GPIO_EDGE:
		ret = p->edge_offset;
		break;
	case GPIO_RMASK:
		ret = p->rmask_offset;
		break;
	case GPIO_MASK:
		ret = p->fmask_offset;
		break;
	case GPIO_DATA:
		ret = p->data_offset;
		break;
	case GPIO_DIR:
		ret = p->dir_offset;
		break;
	case GPIO_POL:
		ret = p->pol_offset;
		break;
	case GPIO_SET:
		ret = p->set_offset;
		break;
	case GPIO_RESET:
		ret = p->reset_offset;
		break;
	default:
		return 0;
	}

	ret += RT2880_PRGIO_ADDR;

	return ret;
}

/* Set GPIO pin direction.
 * If a GPIO pin is multi-function pin, it would be configured as GPIO mode.
 * @gpio_nr:	GPIO number
 * @gpio_dir:	GPIO direction
 * 	1: 	output
 * 	0:	input
 *  otherwise:	input
 * @return:
 * 	0:	Success
 * 	-1:	Invalid parameter
 */
int mtk_set_gpio_dir(unsigned short gpio_nr, unsigned short gpio_dir_out)
{
	int shift;
	unsigned int msk, val;
	unsigned int reg;

	if (!is_valid_gpio_nr(gpio_nr))
		return -1;

	reg = get_gpio_reg_addr(gpio_nr, GPIO_DIR);
	shift = get_gpio_reg_bit_shift(gpio_nr);

	if (gpio_dir_out) {
		/* output */
		ra_or(reg, 1U << shift);
	} else {
		/* input */
		ra_and(reg, ~(1U << shift));
	}

	/* Handle special GPIO pin */
	shift = -1;
	msk = val = 1;
#if defined(RT3883_MP)
	if (gpio_nr >= 17 && gpio_nr <= 21) {
		/* JTAG */
		shift = 6;
#if !defined(ON_BOARD_SPI_FLASH_COMPONENT)
	} else if (gpio_nr >= 3 && gpio_nr <= 6) {
		/* SPI */
		shift = 1;
#endif
	} else if (gpio_nr >= 22 && gpio_nr <= 23) {
		/* MDIO */
		shift = 7;
#if !defined(RT3883_USE_GE1)
	} else if (gpio_nr >= 84 && gpio_nr <= 95) {
		/* RGMII1 */
		shift = 9;
#endif
#if !defined(RT3883_USE_GE2)
	} else if (gpio_nr >= 72 && gpio_nr <= 83) {
		/* RGMII2 */
		shift = 10;
#endif
	} else if (gpio_nr >= 32 && gpio_nr <= 34) {
		/* LNA_A */
		shift = 16;
		msk = val = 3;
	} else if (gpio_nr >= 35 && gpio_nr <= 37) {
		/* LNA_G */
		shift = 18;
		msk = val = 3;
	}
#elif defined(MT7620_MP)
	if (gpio_nr >= 18 && gpio_nr <= 21) {
		/* PA */
		shift = 20;
#if !defined (P5_RGMII_TO_MAC_MODE)
	} else if (gpio_nr >= 22 && gpio_nr <= 23) {
		/* MDIO */
		shift = 7;
		msk = val = 3;
#endif
	} else if (gpio_nr >= 24 && gpio_nr <= 35) {
		/* RGMII1 */
		shift = 9;
	} else if (gpio_nr >= 60 && gpio_nr <= 71) {
		/* RGMII2 */
		shift = 10;
#if !defined(ON_BOARD_SPI_FLASH_COMPONENT)
	} else if (gpio_nr >= 3 && gpio_nr <= 6) {
		/* SPI */
		shift = 11;
#endif
	} else if (gpio_nr >= 40 && gpio_nr <= 44) {
		/* JTAG/EPHY_LED */
		shift = 15;
	} else if (gpio_nr == 36) {
		/* PERST */
		shift = 16;
		msk = val = 3;
#if !defined(ON_BOARD_NAND_FLASH_COMPONENT)
	} else if (gpio_nr >= 45 && gpio_nr <= 59) {
		/* NAND/SD_BT */
		shift = 18;
		msk = val = 3;
#endif
	} else if (gpio_nr == 72) {
		/* WLED */
		shift = 13;
	}
#elif defined(MT7621_MP)
	if (gpio_nr >= 22 && gpio_nr <= 33) {
		/* RGMII2 */
		shift = 15;
#if !defined(ON_BOARD_NAND_FLASH_COMPONENT)
	} else if (gpio_nr >= 41 && gpio_nr <= 48) {
		/* SDXC/NAND */
		shift = 18;
		msk = 3;
		val = 1;
#endif
	} else if (gpio_nr == 61) {
		/* ESW INT */
		shift = 20;
	}
#elif defined(MT7628_MP)
	msk = 3;
	if (gpio_nr >= 0 && gpio_nr <= 3) {
		/* I2S */
		shift = 6;
	} else if (gpio_nr >= 4 && gpio_nr <= 5) {
		/* I2C */
		shift = 20;
	} else if (gpio_nr == 6) {
		/* SPI_CS1 */
		shift = 4;
	} else if (gpio_nr == 11) {
		/* GPIO */
		shift = 0;
	} else if (gpio_nr >= 14 && gpio_nr <= 17) {
		/* SPIS */
		shift = 2;
	} else if (gpio_nr == 18) {
		/* PWM0/eMMC */
		shift = 28;
	} else if (gpio_nr == 19) {
		/* PWM1/eMMC */
		shift = 30;
	} else if (gpio_nr >= 20 && gpio_nr <= 21) {
		/* UART3/eMMC */
		shift = 26;
	} else if (gpio_nr >= 22 && gpio_nr <= 29) {
		/* SDXC/eMMC */
		shift = 10;
	} else if (gpio_nr == 36) {
		/* PERST */
		shift = 16;
	} else if (gpio_nr == 37) {
		/* REFCLK */
		shift = 18;
	} else if (gpio_nr == 38) {
		/* WDT */
		shift = 14;
	} else if (gpio_nr >= 45 && gpio_nr <= 46) {
		/* UART2 */
		shift = 24;
	}
#endif
	if (shift >= 0) {
		reg = ra_inl(RT2880_GPIOMODE_REG);
		reg &= ~(msk << shift);
		reg |=  (val << shift);
		ra_outl(RT2880_GPIOMODE_REG, reg);
	}

	return 0;
}

/* Read GPIO pin value.
 * @gpio_nr:	GPIO number
 * @return:	GPIO value
 * 	0/1:	Success
 * 	-1:	Invalid parameter
 */

int mtk_get_gpio_pin(unsigned short gpio_nr)
{
	int shift;
	unsigned int reg;
	unsigned int val = 0;

	if (!is_valid_gpio_nr(gpio_nr))
		return -1;

	reg = get_gpio_reg_addr(gpio_nr, GPIO_DATA);
	shift = get_gpio_reg_bit_shift(gpio_nr);

	val = ra_inl(reg);
	val = (val >> shift) & 1U;

	return val;
}

/* Set GPIO pin value
 * @gpio_nr:	GPIO number
 * @val:
 * 	0:	Write 0 to GPIO pin
 *  otherwise:	Write 1 to GPIO pin
 * @return:
 * 	0:	Success
 * 	-1:	Invalid parameter
 */

int mtk_set_gpio_pin(unsigned short gpio_nr, unsigned int val)
{
	int shift;
	unsigned int reg;

	if (!is_valid_gpio_nr(gpio_nr))
		return -1;

	reg = get_gpio_reg_addr(gpio_nr, (val) ? GPIO_SET : GPIO_RESET);
	shift = get_gpio_reg_bit_shift(gpio_nr);

	ra_outl(reg, (1U << shift));

	return 0;
}

void gpio_init(void)
{
	unsigned int gm = RALINK_GPIOMODE_I2C;
#if defined(RT3052_MP2) || defined(RT3352_MP) || defined(RT5350_MP) || defined(RT3883_MP)
	gm |= RALINK_GPIOMODE_UARTF|RALINK_GPIOMODE_JTAG;
#elif defined(MT7620_MP)
	gm |= RALINK_GPIOMODE_UARTF|RALINK_GPIOMODE_WDT|RALINK_GPIOMODE_SPI_REFCLK;
#elif defined(MT7621_MP)
	/* AX1800 的 reset(16)/hdog(14)/usbpower(13)/sysLed(17) 全在 JTAG 组(13~17),
	 * JTAG+UART2+UART3+WDT 统一切 GPIO 模式(量产无 JTAG 需求, 无损) */
	gm |= RALINK_GPIOMODE_UART2|RALINK_GPIOMODE_UART3|RALINK_GPIOMODE_WDT|RALINK_GPIOMODE_JTAG;
#elif defined(MT7628_MP)
	gm |= RALINK_GPIOMODE_UART2|RALINK_GPIOMODE_UART3|RALINK_GPIOMODE_WDT;
#endif
	ra_or(RT2880_GPIOMODE_REG, gm);

#if defined(GPIO_LED_ALL)
#if (GPIO_LED_ALL >= 0)
	mtk_set_gpio_dir(GPIO_LED_ALL, GPIO_DIR_OUTPUT);
	mtk_set_gpio_pin(GPIO_LED_ALL, GPIO_VAL_LED_SHOW);
#endif
#endif

#if defined(GPIO_LED_POWER)
#if (GPIO_LED_POWER >= 0)
	mtk_set_gpio_dir(GPIO_LED_POWER, GPIO_DIR_OUTPUT);
	mtk_set_gpio_pin(GPIO_LED_POWER, GPIO_VAL_LED_SHOW);
#endif
#endif

	/* prepare all buttons */
#if defined(GPIO_BTN_RESET)
#if (GPIO_BTN_RESET >= 0)
	mtk_set_gpio_dir(GPIO_BTN_RESET, GPIO_DIR_INPUT);
#endif
#endif

	/* AX1800: USB 口供电(GPIO13, 低=上电), uboot 阶段保持供电, 由 OS 接管 */
#if defined(GPIO_USBPOWER)
#if (GPIO_USBPOWER >= 0)
	printf("Board: %s (GPIO%d usb power %s=on), keep power ON in uboot\n",
		BOARD_NAME_STR, GPIO_USBPOWER,
		GPIO_USBPOWER_ON_LEVEL ? "high" : "low");
	mtk_set_gpio_dir(GPIO_USBPOWER, GPIO_DIR_OUTPUT);
	mtk_set_gpio_pin(GPIO_USBPOWER, GPIO_USBPOWER_ON_LEVEL);
#else
	printf("Board: %s (usb power not driven by uboot)\n", BOARD_NAME_STR);
#endif
#endif

	/* 看门狗喂狗脚方向预置(输出), 上电先喂一次防复位 */
#if (GPIO_HW_WDT >= 0)
	mtk_set_gpio_dir(GPIO_HW_WDT, GPIO_DIR_OUTPUT);
	mtk_set_gpio_pin(GPIO_HW_WDT, 0);
#endif
}

void gpio_init_mdio(void)
{
#if defined(RALINK_GPIOMODE_MDIO)
	ra_and(RT2880_GPIOMODE_REG, (~RALINK_GPIOMODE_MDIO));
#endif
}

int DETECT_BTN_RESET(void)
{
	int key = 0;
#if defined(GPIO_BTN_RESET)
#if (GPIO_BTN_RESET >= 0)
	if (mtk_get_gpio_pin(GPIO_BTN_RESET) == GPIO_VAL_BTN_PRESSED) {
		key = 1;
		/* printf("RESET button pressed!\n"); */
	}
#endif
#endif
	return key;
}

void LED_INIT(void)
{
	int i = 0;
	int led[] = {POWER_LED, WAN_LED, WLAN_LED};

	for (i = 0; i < ARRAY_SIZE(led); i++) {
		mtk_set_gpio_dir(led[i], GPIO_DIR_OUTPUT);
	}
}

void ALL_LED_STATUS(enum led_status status)
{
	int i = 0;
	int led[] = {POWER_LED, WAN_LED, WLAN_LED};

	for (i = 0; i < ARRAY_SIZE(led); i++) {
		mtk_set_gpio_pin(led[i], status);
	}
}

void LED_STATUS(enum led_gpio led, enum led_status status)
{
	return mtk_set_gpio_pin(led, status);
}

void wan_led_toggle(void)
{
	int st = 0;

	st = mtk_get_gpio_pin(WAN_LED);
	mtk_set_gpio_pin(WAN_LED, !st);
}

/* run 灯心跳(慢闪/快闪): 由倒计时/NetLoop/raspi 擦写等轮询点周期性调用。
 * 用 get_timer 防抖, 到 period_ms 才翻转一次 -> 与调用频率解耦。
 * 翻转用内部状态变量, 不读回引脚 —— PIO 的 DATA(0x20) 读的是引脚
 * 实际电平, 低有效 LED + 外部驱动场合读回值可能恒定, "读回取反"式
 * 翻转会退化成恒写同一电平。RUN_LED<0(无该灯)时空操作。 */
void run_led_heartbeat(unsigned int period_ms)
{
	static unsigned long last = 0;
	static int led_on = 0;		/* 内部状态, 与硬件输出同步 */
	unsigned long now;

	if (RUN_LED < 0)
		return;

	now = get_timer(0);
	if ((now - last) >= period_ms) {
		led_on = !led_on;
		mtk_set_gpio_pin(RUN_LED, led_on);
		last = now;
	}
}

/* 硬件看门狗喂狗(AX1800 GPIO#14)。看门狗语义(硬件确认): 2 分钟内需收到
 * 一个 ≥50ms 的电平脉冲(高电平保持 ≥50ms), 不是边沿触发 —— 瞬态翻转
 * 喂不上狗。喂狗 = 常态低电平, 拉高保持 100ms 再拉低(一个完整脉冲)。
 * 脉冲期间的 100ms 忙等避开 SPI 事务即可(调用点均在阶段切换/收包循环
 * 等空闲边界)。无看门狗单板(GPIO_HW_WDT=-1)时为空操作。 */
#define WDT_PULSE_MS		100	/* 脉冲宽度, 看门狗要求 ≥ 50ms */

/* 发一个喂狗脉冲: 低->高(保持100ms)->低。阻塞 100ms, 不要在 SPI 事务中调用。
 * 调用点: reset 按键检测前 / raspi_erase_write 的 E/W/V 阶段切换 /
 * httpd 上传完成 / bootm 启动内核前。 */
void wdt_gpio_kick(void)
{
#if (GPIO_HW_WDT >= 0)
	mtk_set_gpio_dir(GPIO_HW_WDT, GPIO_DIR_OUTPUT);
	mtk_set_gpio_pin(GPIO_HW_WDT, 1);
	udelay(WDT_PULSE_MS * 1000);
	mtk_set_gpio_pin(GPIO_HW_WDT, 0);
#endif
}

/* 错误指示: RUN 灯"三短一长"循环闪烁(SOS 风格, 长亮 800ms 短亮 200ms,
 * 各间隔 200ms, 组间停 1s)。
 * 用途: bootm 校验失败(Bad Data CRC)等致命错误掉回命令行后, 命令行
 * readline 等待输入时无人驱动 LED(heartbeat 是被动轮询), 用本函数
 * 主动阻塞闪烁, 现场看灯即知"固件损坏, 停在命令行"。
 * 注意: 阻塞期间不响应串口(每次组间间隔很短, 可 Ctrl+C 后由 readline
 * 正常接管)。永远不返回 —— 设备需要人工处理。 */
void run_led_error_blink(void)
{
	int cycle;

	if (RUN_LED < 0) {
		for (;;)
			;
	}

	for (cycle = 0; ; cycle++) {
		int i;
		/* 三短 */
		for (i = 0; i < 3; i++) {
			mtk_set_gpio_pin(RUN_LED, 1);
			udelay(200 * 1000);
			mtk_set_gpio_pin(RUN_LED, 0);
			udelay(200 * 1000);
		}
		/* 一长 */
		mtk_set_gpio_pin(RUN_LED, 1);
		udelay(800 * 1000);
		mtk_set_gpio_pin(RUN_LED, 0);
		/* 组间停顿 */
		udelay(1000 * 1000);
	}
}
