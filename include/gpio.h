#ifndef _GPIO_H_
#define _GPIO_H_

enum led_status {
	LED_ON=0,
	LED_OFF=1
};

enum led_gpio {
#if defined(MT7621_MP)
#  if defined(BOARD_AX1800)
	/* AX1800 (MT7621) GPIO/LED 映射:
	 *   GPIO#16 reset 键(低有效) / GPIO#13 usbpower(低=上电) / GPIO#14 hdog 看门狗
	 *     —— 见 drivers/gpio.c 的 GPIO_BTN_RESET / GPIO_USBPOWER / GPIO_HW_WDT。
	 *   LED 两颗受 uboot 控制:
	 *     sysLed = RUN_LED = GPIO#17: uboot 全程常亮("系统在跑"指示), 内核起来后由系统接管;
	 *     wan    = WAN_LED = GPIO#15: 升级状态灯(承担 mt7628 里 RUN 灯的角色) ——
	 *              按住 reset 慢闪 / 入口确认闪烁(网页 3 慢、强刷 6 快) / TFTP 下载慢闪 /
	 *              httpd 等待常亮 / 上传与写 flash 快闪 / bootm 校验失败三短一长,
	 *              离开升级流程即灭。run_led_heartbeat() 驱动的就是这颗。
	 *   wwan2Led=GPIO#18 由 Linux 驱动, uboot 置输出+灭(WWAN2_LED)。
	 *   默认低电平=亮(输出 0 点亮); 高有效时在 rt2880.h 定义 GPIO_LED_INVERTED 即可整体翻转。
	 *   POWER_LED=-1: 本板无独立 power 灯(rt2880.h 同时禁用 legacy LED_POWER 路径)。
	 *   -1 经 is_valid_gpio_nr() 判为无效, mtk_set_gpio_* 为安全空操作。 */
	POWER_LED=-1,
	WAN_LED=15,
	WLAN_LED=-1,
	RUN_LED=17,
	WWAN2_LED=18
#  else
	/* FireWRT 原板: POWER LED = GPIO#22 (RGMII2 复用组) */
	POWER_LED=22,
	WAN_LED=22,
	WLAN_LED=-1,
	RUN_LED=22,
	WWAN2_LED=-1
#  endif
#elif defined(MT7628_MP)
	POWER_LED=0,
	WAN_LED=0,
	WLAN_LED=0,
	RUN_LED=0,
	WWAN2_LED=-1
#else
	POWER_LED=0,
	WAN_LED=0,
	WLAN_LED=0,
	RUN_LED=0,
	WWAN2_LED=-1
#endif
};


extern void gpio_init(void);
extern void gpio_init_mdio(void);
extern void gpio_init_usb(int do_wait);

extern int mtk_set_gpio_dir(unsigned short gpio_nr, unsigned short gpio_dir_out);
extern int mtk_get_gpio_pin(unsigned short gpio_nr);
extern int mtk_set_gpio_pin(unsigned short gpio_nr, unsigned int val);

extern int DETECT_BTN_RESET(void);
extern int DETECT_BTN_WPS(void);

extern void LED_HIDE_ALL(void);
extern void LED_POWER_ON(void);

extern void LED_ALERT_ON(void);
extern void LED_ALERT_OFF(void);
extern void LED_ALERT_BLINK(void);

extern void LED_INIT(void);
extern void ALL_LED_STATUS(enum led_status status);
extern void LED_STATUS(enum led_gpio led, enum led_status status);

extern void wan_led_toggle(void);

/* 升级状态灯(WAN_LED)心跳: 由各轮询点(NetLoop TFTP 下载 / httpd 收包上传 / raspi 擦写)
 * 周期性调用, 按 period_ms 翻转 WAN_LED, 实现"慢闪/快闪"。sysLed(RUN_LED)不参与闪烁,
 * 全程常亮。用 get_timer 防抖, 与调用频率无关; WAN_LED<0(无该灯)时空操作。 */
extern void run_led_heartbeat(unsigned int period_ms);
#define RUN_LED_BLINK_SLOW	400	/* ms, 下载阶段慢闪 (~1.2Hz) */
#define RUN_LED_BLINK_FAST	100	/* ms, 升级上传/写 flash 快闪 (~5Hz) */

/* 硬件看门狗喂狗(AX1800 GPIO#14): 2 分钟内需一个 ≥50ms 的电平脉冲(非边沿触发)。
 * wdt_gpio_kick: 发一个完整脉冲(低->高100ms->低), 阻塞 100ms。
 * 调用点: reset 按键检测前 / TFTP 安装写 flash 的 E/W/V 阶段切换 /
 * httpd 上传完成 / bootm 启动内核前 / 命令行空闲每 60s。 */
extern void wdt_gpio_kick(void);

/* 错误指示: WAN 灯(GPIO15)"三短一长"循环(短 200ms×3 + 长 800ms, 组间停 1s)。
 * bootm 校验失败(Bad Data CRC)等致命错误掉回命令行时调用 —— 命令行
 * 等待输入期间无人驱动 LED, 用本函数主动阻塞闪烁指示错误。永不返回。 */
extern void run_led_error_blink(void);

#endif
