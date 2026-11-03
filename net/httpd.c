/*
 *	Copyright 1994, 1995, 2000 Neil Russell.
 *	(See License)
 *	Copyright 2000, 2001 DENX Software Engineering, Wolfgang Denk, wd@denx.de
 */

#include <common.h>
#include <command.h>
#include <net.h>
#include <asm/byteorder.h>
#include "httpd.h"

#include "../httpd/uipopt.h"
#include "../httpd/uip.h"
#include "../httpd/uip_arp.h"

#define BUF ((struct uip_eth_hdr *)&uip_buf[0])

#include <spi_api.h>

extern int env_reload_for_save(void);	/* common/env_common.c: 从 flash 重载并自愈 env(CRC坏/内容垃圾回退默认) */
extern int board_kernel_image_ok(void);	/* common/main.c: 校验 flash 内核 uImage(magic+双CRC) 是否有效 */
extern void factory_crc_print(const char *tag);	/* lib_mips/board.c: 只读打印 factory 整区 CRC32, 升级写 flash 前后留证 */
extern void env_crc_print(const char *tag);	/* lib_mips/board.c: 只读打印 env 区 CRC32+摘要, 排查凭证是否落在 env */

/* 字符串化配置宏(同 env_common.c): CONFIG_SERVERIP 等是数字字面量,
 * setenv 需要字符串形式 */
#define XMK_STR(x)	#x
#define MK_STR(x)	XMK_STR(x)

static int arptimer = 0;

void HttpdHandler( void ){
	int i;

	if ( uip_len == 0 ) {
		for ( i = 0; i < UIP_CONNS; i++ ) {
			uip_periodic( i );
			if ( uip_len > 0 ) {
				uip_arp_out();
				NetSendHttpd();
			}
		}

		if ( ++arptimer == 20 ) {
			uip_arp_timer();
			arptimer = 0;
		}
	} else {
		if ( BUF->type == htons( UIP_ETHTYPE_IP ) ) {
			uip_arp_ipin();
			uip_input();
			if ( uip_len > 0 ) {
				uip_arp_out();
				NetSendHttpd();
			}
		} else if ( BUF->type == htons( UIP_ETHTYPE_ARP ) ) {
			uip_arp_arpin();
			if ( uip_len > 0 ) {
				NetSendHttpd();
			}
		}
	}

}

// start http daemon
void HttpdStart( void ){
	uip_init();
	httpd_init();
}

int do_http_upgrade( const ulong size, const int upgrade_type ){
	DECLARE_GLOBAL_DATA_PTR;

	if ( upgrade_type == WEBFAILSAFE_UPGRADE_TYPE_UBOOT ) {

		printf( "\n\n****************************\n*     U-BOOT UPGRADING     *\n* DO NOT POWER OFF DEVICE! *\n****************************\n\n" );

		/* 防变砖硬性防线(执行层): uboot 镜像超过 192KB(CFG_BOOTLOADER_SIZE=0x30000)
		 * 一律拒刷 —— 写入会溢出 bootloader 分区, 冲掉 env(0x30000)甚至更远, 设备变砖。
		 * httpd/httpd.c 上传入口已有同款检查(L220), 此处兜底: 任何路径调到这里都拦得住。 */
		if ( size > WEBFAILSAFE_UPLOAD_UBOOT_SIZE_IN_BYTES ) {
			printf( "## Error: uboot image %lu bytes > %d (bootloader partition 0x0~0x2FFFF) -> REFUSED, flash untouched!\n",
				size, WEBFAILSAFE_UPLOAD_UBOOT_SIZE_IN_BYTES );
			return( -1 );
		}

		{
			/* 按实际上传大小写入(不再固定 192KB): raspi_erase_write 对文件尾部
			 * 非 64KB 对齐部分走块级读-改-写, 文件之外的分区尾部保留原内容,
			 * 与原厂菜单 9 行为一致; env(0x30000)/factory(0x40000) 均不受影响。
			 * (httpd/httpd.c 入口已限制上传文件 <= 192KB) */
			int rv;

			/* factory/env CRC 留证打印已停用(排查结论: 升级写入不波及两区),
			 * 函数保留, 需要时恢复调用即可:
			factory_crc_print( "http-uboot-before" );
			env_crc_print( "http-uboot-before" );
			*/
			rv = raspi_erase_write( ( u8_t * )WEBFAILSAFE_UPLOAD_RAM_ADDRESS, WEBFAILSAFE_UPLOAD_UBOOT_ADDRESS - CFG_FLASH_BASE, ( int )size );
			/*
			env_crc_print( "http-uboot-after" );
			factory_crc_print( "http-uboot-after" );
			*/

			if ( rv == 0 ) {
				/* 按需求重新启用: uboot 写入成功后把关键启动项刷成编译期
				 * 默认值(rt2880.h)。背景: 原厂 MTK 时代 env 残留
				 * (bootcmd=tftp / 旧网段)会让新 uboot 行为异常; fwupdated
				 * 方案已停用(不存在"强制 bootfile 触发首启 TFTP 重刷
				 * kernel"的联动), 只对齐这 4 项, 其余条目(ethaddr 等)原样保留。
				 * debug: 先打印本二进制实际携带的编译期值 —— 强制写入的
				 * 是"执行方"的宏, 若烧的是改 IP 前编译的旧 bin, 写入的
				 * 就是旧网段(鸡生蛋: 需再多烧一次新 bin 才带新值)。 */
				printf( "## http-upgrade: forcing boot env (built-in defaults):\n"
					"##   bootcmd=%s\n##   serverip=%s\n##   ipaddr=%s\n##   bootfile=%s\n",
					CONFIG_BOOTCOMMAND, MK_STR(CONFIG_SERVERIP),
					MK_STR(CONFIG_IPADDR), CONFIG_BOOTFILE );
				env_reload_for_save();	/* 重载+自愈, 防止踩坏的 env 存回 flash */
				setenv( "bootcmd", CONFIG_BOOTCOMMAND );
				setenv( "serverip", MK_STR(CONFIG_SERVERIP) );
				setenv( "ipaddr",   MK_STR(CONFIG_IPADDR) );
				/* CONFIG_BOOTFILE 本身是字符串字面量, 不经 MK_STR ——
				 * 否则二次包裹成 "..." 带引号, TFTP 请求文件名匹配不上 */
				setenv( "bootfile", CONFIG_BOOTFILE );
				saveenv();

				/* debug: saveenv 后从 flash 读回, 打印 4 项实际落盘值,
				 * 区分"没写进 flash"与"写进了但值是旧编译期的" */
				{
					char *chk = (char *)malloc(CFG_ENV_SIZE);
					if ( chk && raspi_read(chk, CFG_ENV_ADDR - CFG_FLASH_BASE, CFG_ENV_SIZE)
							== CFG_ENV_SIZE ) {
						static const char *keys[] = { "bootcmd", "serverip", "ipaddr", "bootfile" };
						char *p;
						printf( "## http-upgrade: env saved, values in flash now:\n" );
						/* 条目区在 CRC(4字节)之后, 每条 "name=value\0", 双'\0'结尾 */
						for ( p = chk + 4; p < chk + CFG_ENV_SIZE && *p; p += strlen(p) + 1 ) {
							int k;
							for ( k = 0; k < 4; k++ ) {
								if ( !strncmp(p, keys[k], strlen(keys[k]))
									&& p[strlen(keys[k])] == '=' ) {
									printf( "##   %s\n", p );
									break;
								}
							}
						}
					} else {
						printf( "## http-upgrade: env read-back FAILED (verify skipped)\n" );
					}
					if ( chk )
						free( chk );
				}
				printf( "## http-upgrade: boot env forced to defaults & saved\n" );

				/* fwupdated 标志已停用(不再参与任何决策, 见 main.c):
				 * 上电一律直接本地启动, TFTP 只由 reset >8s 显式触发。
				 * kernel 校验/置标逻辑保留注释备查。 */
#if 0
				printf( "## http-upgrade: uboot flashed, checking kernel (takes a few seconds)...\n" );
				if ( board_kernel_image_ok() ) {
					printf( "## http-upgrade: kernel OK, marking fwupdated=1\n" );
					setenv( "fwupdated", "1" );
					saveenv();
				} else {
					printf( "## http-upgrade: kernel INVALID, fwupdated not set "
						"(next boot will try TFTP install)\n" );
				}
#endif
			}

			return( rv );
		}

	} else if ( upgrade_type == WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE ) {
		int rv;

		printf( "\n\n****************************\n*    FIRMWARE UPGRADING    *\n* DO NOT POWER OFF DEVICE! *\n****************************\n" );
		printf( "flash kernel @ 0x%08X, %lu bytes\n\n", (unsigned)WEBFAILSAFE_UPLOAD_KERNEL_ADDRESS, size );
		/* CRC 留证打印已停用(排查结论: 升级写入不波及两区), 需要时恢复:
		factory_crc_print( "http-fw-before" );
		env_crc_print( "http-fw-before" );
		*/
		rv = raspi_erase_write( ( u8_t * )WEBFAILSAFE_UPLOAD_RAM_ADDRESS, WEBFAILSAFE_UPLOAD_KERNEL_ADDRESS - CFG_FLASH_BASE, size );
		/*
		env_crc_print( "http-fw-after" );
		factory_crc_print( "http-fw-after" );
		*/

		/* fwupdated 标志已停用(不再参与任何决策, 见 main.c):
		 * 上电一律直接本地启动, TFTP 只由 reset >8s 显式触发。
		 * 原置标逻辑保留注释备查。 */
#if 0
		if ( rv == 0 ) {
			printf( "## http-upgrade: flash write OK, reload env...\n" );
			env_reload_for_save();
			setenv( "fwupdated", "1" );
			printf( "## http-upgrade: setenv done, saveenv...\n" );
			saveenv();
			printf( "## http-upgrade: env saved, rebooting...\n" );
		}
#endif

		return( rv );

	} else if ( upgrade_type == WEBFAILSAFE_UPGRADE_TYPE_ART ) {
		int rv;

		printf( "\n\n****************************\n*      ART  UPGRADING      *\n* DO NOT POWER OFF DEVICE! *\n****************************\n\n" );
		/* CRC 留证打印已停用(ART 写的就是 factory, 前后不同属预期), 需要时恢复:
		factory_crc_print( "http-art-before" );
		*/
		rv = raspi_erase_write( ( u8_t * )WEBFAILSAFE_UPLOAD_RAM_ADDRESS, WEBFAILSAFE_UPLOAD_ART_ADDRESS - CFG_FLASH_BASE, WEBFAILSAFE_UPLOAD_ART_SIZE_IN_BYTES );
		/*
		factory_crc_print( "http-art-after" );
		*/
		return( rv );

	} else {
		return(-1);
	}

	return(-1);
}

// info about current progress of failsafe mode
int do_http_progress( const int state ){
	unsigned char i = 0;

	/* toggle LED's here */
	switch ( state ) {
		case WEBFAILSAFE_PROGRESS_START:

			// blink LED fast 10 times
			for ( i = 0; i < 10; ++i ) {
				/* LEDON(); */
				udelay( 25000 );
				/* LEDOFF(); */
				udelay( 25000 );
			}

			printf( "HTTP server is ready!\n\n" );
			break;

		case WEBFAILSAFE_PROGRESS_TIMEOUT:
			//printf("Waiting for request...\n");
			break;

		case WEBFAILSAFE_PROGRESS_UPLOAD_READY:

			// blink LED fast 10 times
			for ( i = 0; i < 10; ++i ) {
				/* LEDON(); */
				udelay( 25000 );
				/* LEDOFF(); */
				udelay( 25000 );
			}
			printf( "HTTP upload is done! Upgrading...\n" );
			break;

		case WEBFAILSAFE_PROGRESS_UPGRADE_READY:

			// blink LED fast 10 times
			for ( i = 0; i < 10; ++i ) {
				/* LEDON(); */
				udelay( 25000 );
				/* LEDOFF(); */
				udelay( 25000 );
			}
			printf( "HTTP ugrade is done! Rebooting...\n\n" );
			break;

		case WEBFAILSAFE_PROGRESS_UPGRADE_FAILED:
			printf( "## Error: HTTP ugrade failed!\n\n" );

			// blink LED fast for 4 sec
			for ( i = 0; i < 80; ++i ) {
				/* LEDON(); */
				udelay( 25000 );
				/* LEDOFF(); */
				udelay( 25000 );
			}

			// wait 1 sec
			udelay( 1000000 );

			break;
	}

	return( 0 );
}
