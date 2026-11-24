/*
 * (C) Copyright 2003
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <common.h>

extern unsigned long mips_cpu_feq;

static void mips_compare_set(u32 v)
{
	asm volatile ("mtc0 %0, $11" : : "r" (v));
}

static void mips_count_set(u32 v)
{
	asm volatile ("mtc0 %0, $9" : : "r" (v));
}

static u32 mips_count_get(void)
{
	u32 count;

	asm volatile ("mfc0 %0, $9" : "=r" (count) :);
	return count;
}

/*
 * timer without interrupts
 */
int timer_init(void)
{
	
	mips_compare_set(0);
	mips_count_set(0);
	
	return 0;
}


ulong get_timer(ulong base)
{
	//printf("%s = %x\n", __FUNCTION__, mips_count_get() );
	return mips_count_get() - base;
}

/* CP0 Count 原始计数 -> 毫秒。
 * 本树 get_timer() 返回 mips_count_get() 差值(Count 按 CPU/2 递增), 不是 ms;
 * 需要 ms 语义的调用点: 先 get_timer(base) 做无符号差(天然处理 32 位 Count
 * 约 9.75s 一次的回绕), 再经此换算; 不要拿 get_timer 返回值直接当 ms 比较。 */
ulong ticks_to_ms(ulong ticks)
{
	return ticks / ((mips_cpu_feq / 2) / 1000);
}


void udelay (unsigned long usec)
{
	ulong tmo;
	ulong start = get_timer(0);

	tmo = usec * ((mips_cpu_feq/2) / 1000000);
	while ((ulong)((mips_count_get() - start)) < tmo)
		/*NOP*/;
}

void mdelay(unsigned long msec)
{
	while (msec--)
		udelay(1000);
}

#if 0
/*
 * This function is derived from PowerPC code (read timebase as long long).
 * On MIPS it just returns the timer value.
 */
unsigned long long get_ticks(void)
{
	return mips_count_get();
}

/*
 * This function is derived from PowerPC code (timebase clock frequency).
 * On MIPS it returns the number of timer ticks per second.
 */
ulong get_tbclk(void)
{
	return CFG_HZ;
}

void reset_timer(void)
{
	mips_count_set(0);
}

void set_timer(ulong t)
{
	mips_count_set(t);
}
#endif
