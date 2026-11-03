#include <common.h>	/* DECLARE_GLOBAL_DATA_PTR / gd->ram_size(设备信息替换用) */
#include "uip.h"
#include "httpd.h"
#include "fs.h"
#include "fsdata.h"

/* drivers/spi_rt6855A_flash.c: 上电 JEDEC 探测结果(firmware 大小校验用) */
struct chip_info {
	char		*name;
	unsigned char	id;
	unsigned int	jedec_id;
	unsigned long	sector_size;
	unsigned int	n_sectors;
	char		addr4b;
};
extern struct chip_info *spi_chip_info;

#define STATE_NONE				0		// empty state (waiting for request...)
#define STATE_FILE_REQUEST		1		// remote host sent GET request
#define STATE_UPLOAD_REQUEST	2		// remote host sent POST request

// ASCII characters
#define ISO_G					0x47	// GET
#define ISO_E					0x45
#define ISO_T					0x54
#define ISO_P					0x50	// POST
#define ISO_O					0x4f
#define ISO_S					0x53
#define ISO_T					0x54
#define ISO_slash				0x2f	// control and other characters
#define ISO_space				0x20
#define ISO_nl					0x0a
#define ISO_cr					0x0d
#define ISO_tab					0x09

// we use this so that we can do without the ctype library
#define is_digit(c)				((c) >= '0' && (c) <= '9')

// boc: httpd 下载进度打印节流 —— 每 N 个收到的包才打印一个 '#', 避免低波特率下
// per-packet puts("#") 阻塞饿死 uIP 收包(大文件上传卡死)。N=16 约 1/16 串口负载。
#define HTTPD_PROGRESS_PRINT_DIV	16

// debug
//#define DEBUG_UIP

// html files
extern const struct fsdata_file file_index_html;
extern const struct fsdata_file file_404_html;
extern const struct fsdata_file file_flashing_html;
extern const struct fsdata_file file_fail_html;

extern int webfailsafe_ready_for_upgrade;
extern int webfailsafe_uploading;	/* net.c: 正在接收固件上传, 主循环据此喂狗+快闪 */
extern int webfailsafe_upgrade_type;
extern ulong NetBootFileXferSize;
extern unsigned char *webfailsafe_data_pointer;

// extern flash_info_t flash_info[];

// http app state
struct httpd_state *hs;

static int webfailsafe_post_done = 0;
static ulong webfailsafe_post_tick = 0;	/* post_done 置位时刻, 收尾超时兜底用 */
static int webfailsafe_upload_failed = 0;
static int data_start_found = 0;

/* 上传收尾超时兜底(NetLoopHttpd 主循环周期调用):
 * 数据收齐后, 正常路径是浏览器 ACK 完 flashing 页 -> uip_acked 分支置
 * webfailsafe_ready_for_upgrade。但收尾阶段任何一段回应/ACK 丢一次,
 * 链路静默 -> uIP 重传计时器无人驱动(只在收包时跑) -> 永远等不到,
 * 表现为"#####"后卡死, 要浏览器回退带新包进来才解。
 * 本兜底: post_done 置位后 3 秒(正常 ACK <100ms)仍未就绪且无失败,
 * 直接置 ready_for_upgrade —— 升级照常开始, 浏览器页面可能停在
 * 半截(无碍, 升级由串口 e/w/v 进度可见)。 */
void httpd_upload_stall_check(void)
{
	if (webfailsafe_post_done && webfailsafe_post_tick
	    && !webfailsafe_upload_failed
	    && (get_timer(webfailsafe_post_tick) > 3000)) {
		printf("## httpd: upload-finish ACK stalled %lu ms, forcing upgrade start\n",
		       get_timer(webfailsafe_post_tick));
		webfailsafe_ready_for_upgrade = 1;
		webfailsafe_post_done = 0;
		webfailsafe_post_tick = 0;
	}
}

// 0x0D -> CR 0x0A -> LF
static char eol[3] = { 0x0d, 0x0a, 0x00 };
static char eol2[5] = { 0x0d, 0x0a, 0x0d, 0x0a, 0x00 };

static char *boundary_value;

// str to int
static int atoi(const char *s){
	int i = 0;

	while(is_digit(*s)){
		i = i * 10 + *(s++) - '0';
	}

	return(i);
}

// print downloading progress
static void httpd_download_progress( void ){
	/* boc: 每 HTTPD_PROGRESS_PRINT_DIV 个包才打印一个 '#'.
	 * 原实现每收一个包就 puts("#"), 在低串口波特率(如 57600)下 puts 会阻塞等 TX,
	 * CPU 卡在 eth_rx->uIP 调用链里回不到 NetLoopHttpd 主循环, 下一批以太网包堆在
	 * RX 描述符里被丢弃 -> 大文件上传时 uIP 收不满 upload_total -> 上传卡死、
	 * 不触发 webfailsafe_post_done(看不到 "HTTP upload is done! Upgrading...")。
	 * 节流后串口负载降为 1/N, 不再饿死收包; 换行按 '#' 计(每 32 个 '#' 换行),
	 * 不再按包计数(40 包只有 40/16≈2.5 个 '#', 每行 2-3 个 '#' 就断行难看)。
	 * 注: 原 post_line_counter 到 10 时 wan_led_toggle() 已删 —— WAN_LED 与 RUN_LED
	 * 同一脚, 上传期间 RUN 灯快闪由 NetLoopHttpd 主循环 run_led_heartbeat 统一驱动。 */
	static unsigned int print_div = 0;
	static unsigned char hash_cnt = 0;

	if (++print_div >= HTTPD_PROGRESS_PRINT_DIV) {
		puts("#");
		print_div = 0;
		if (++hash_cnt >= 32) {
			puts("\n");
			hash_cnt = 0;
		}
	}
}

/* 页面设备信息: index.html/uboot.html 内嵌 5 字节占位 "RAMxx"/"ROMxx",
 * 替换为 "%3luMB"( 64MB/128MB/  8MB/ 16MB, 恰好 5 字符等长)。
 * 实测 fsdata 数组在 flash XIP 区(0xBC...., 本树 rodata 不重定位到 RAM),
 * 原地写被静默丢弃(WRITE LOST) —— 改为 malloc 堆副本打补丁,
 * GET 时把 fs_file.data 指针换成副本(len 不变, 等长替换不影响分块发送)。
 * RAM 取 gd->ram_size(启动期实测), Flash 取 JEDEC 探测的芯片容量。
 * 副本各 ~2KB, httpd_init 一次性分配, 整个升级会话复用, 无泄漏。 */
static char *devinfo_index = NULL;	/* "/index.html" 打补丁后的堆副本 */
static char *devinfo_uboot = NULL;	/* "/uboot.html" 打补丁后的堆副本 */

static char *httpd_dup_and_patch(const char *name)
{
	DECLARE_GLOBAL_DATA_PTR;
	struct fs_file file;
	ulong ram_mb = gd->ram_size >> 20;
	ulong flash_mb = 0;
	char ram_str[6], flash_str[6];
	char *buf;
	int i;

	if (!fs_open(name, &file)) {
		printf("## httpd-info: %s NOT FOUND in fs\n", name);
		return NULL;
	}

	buf = (char *)malloc(file.len);
	if (!buf) {
		printf("## httpd-info: no mem for %s copy\n", name);
		return NULL;
	}
	memcpy(buf, file.data, file.len);

	if (spi_chip_info)
		flash_mb = (spi_chip_info->sector_size * spi_chip_info->n_sectors) >> 20;

	sprintf(ram_str, "%3luMB", ram_mb);		/* " 64MB"/"128MB" */
	sprintf(flash_str, "%3luMB", flash_mb);	/* "  8MB"/" 16MB" */

	for (i = 0; i + 5 <= file.len; i++) {
		char *p = buf + i;
		if (p[0] == 'R' && p[1] == 'A' && p[2] == 'M' && p[3] == 'x' && p[4] == 'x') {
			memcpy(p, ram_str, 5);
			i += 4;
		} else if (p[0] == 'R' && p[1] == 'O' && p[2] == 'M' && p[3] == 'x' && p[4] == 'x') {
			memcpy(p, flash_str, 5);
			i += 4;
		}
	}

	printf("## httpd-info: %s served with RAM=%luMB Flash=%luMB\n",
	       name, ram_mb, flash_mb);
	return buf;
}

// http server init
void httpd_init( void ) {
	fs_init();
	uip_listen( HTONS( 80 ) );

	devinfo_index = httpd_dup_and_patch("/index.html");
	devinfo_uboot = httpd_dup_and_patch("/uboot.html");
}

// reset app state
static void httpd_state_reset(void){
	/* 先记录: 本连接是否正处于上传 —— data_start_found/boundary_value/
	 * webfailsafe_uploading 是全局量, 但浏览器常开两条连接(页面+资源各一,
	 * 日志可见两次 "Request for: /"): 闲连接(GET)被关闭时的 reset 若也无
	 * 条件清全局, 会废掉另一条连接正在进行的固件上传(实测表现为
	 * "couldn't find start of data in next packet!" -> 浏览器重试才成功)。
	 * 只有上传连接自己的 reset 才清全局上传状态。 */
	int was_uploading = (hs->state == STATE_UPLOAD_REQUEST);

	hs->state = STATE_NONE;
	hs->count = 0;
	hs->dataptr = 0;
	hs->upload = 0;
	hs->upload_total = 0;

	if (was_uploading) {
		/* 上传结束(完成/失败/断开): 清标志, 主循环停止喂狗+快闪 */
		webfailsafe_uploading = 0;

		data_start_found = 0;

		/* 修复: free 后必须置 NULL。本函数在一次上传流程里会被调多次
		 * (上传完成一次 + flashing 页 ACK 后一次 + 连接关闭一次),
		 * 悬空指针被重复 free 会破坏 dlmalloc 空闲链 -> 后续 raspi_erase_write
		 * 的 malloc(64K) 返回与 env_ptr 重叠的块 -> env 数据被覆写 ->
		 * setenv 扫描越界 -> CPU 异常静默卡死(表现为 Done! 后无任何输出)。 */
		if(boundary_value){
			free(boundary_value);
			boundary_value = NULL;
		}
	}
}

// find and get first chunk of data
static int httpd_findandstore_firstchunk(void){
	char *start = NULL;
	char *end = NULL;
//	flash_info_t *info = &flash_info[0];

	if ( !boundary_value ) {
		return(0);
	}

	// chek if we have data in packet
	start = ( char * )strstr( ( char * )uip_appdata, ( char * )boundary_value );

	if ( start ) {

		// ok, we have data in this packet!
		// find upgrade type

		end = ( char * )strstr( ( char * )start, "name=\"firmware\"" );

		if ( end ) {

			printf( "Upgrade type: firmware\n" );
			webfailsafe_upgrade_type = WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE;

		} else {

			end = ( char * )strstr( ( char * )start, "name=\"uboot\"" );

			if ( end ) {
#if defined(WEBFAILSAFE_DISABLE_UBOOT_UPGRADE)
				printf("## Error: U-Boot upgrade is not allowed on this board!\n");
				webfailsafe_upload_failed = 1;
#else
				webfailsafe_upgrade_type = WEBFAILSAFE_UPGRADE_TYPE_UBOOT;
				printf("Upgrade type: U-Boot\n");
#endif /* if defined(WEBFAILSAFE_DISABLE_UBOOT_UPGRADE) */
			} else {

				end = (char *)strstr((char *)start, "name=\"art\"");

				if(end){
#if defined(WEBFAILSAFE_DISABLE_ART_UPGRADE)
					printf("## Error: U-Boot upgrade is not allowed on this board!\n");
					webfailsafe_upload_failed = 1;
#else
					printf("Upgrade type: ART\n");
					webfailsafe_upgrade_type = WEBFAILSAFE_UPGRADE_TYPE_ART;

					// if we don't have ART partition offset, it means that it should be
					// stored on the last 64 KiB block -> in most supported board
					// the ART partition occupies last 64 KiB block
/* #if !defined(WEBFAILSAFE_UPLOAD_ART_ADDRESS)
					// if we don't know the flash type, we won't allow to update ART,
					// because we don't know flash size
					if(info->flash_id == FLASH_CUSTOM){
						printf("## Error: unknown FLASH type, can't update ART!\n");
						webfailsafe_upload_failed = 1;
					}
#endif */
#endif /* if defined(WEBFAILSAFE_DISABLE_ART_UPGRADE) */
				} else {

					printf("## Error: input name not found!\n");
					return(0);

				}

			}

		}

		end = NULL;

		// find start position of the data!
		end = (char *)strstr((char *)start, eol2);

		if(end){

			if((end - (char *)uip_appdata) < uip_len){

				// move pointer over CR LF CR LF
				end += 4;

				// how much data we expect?
				// last part (magic value 6): [CR][LF](boundary length)[-][-][CR][LF]
				hs->upload_total = hs->upload_total - (int)(end - start) - strlen(boundary_value) - 6;

				printf("Upload file size: %d bytes\n", hs->upload_total);

				// We need to check if file which we are going to download
				// has correct size (for every type of upgrade)

				// U-Boot
				if((webfailsafe_upgrade_type == WEBFAILSAFE_UPGRADE_TYPE_UBOOT) && (hs->upload_total > WEBFAILSAFE_UPLOAD_UBOOT_SIZE_IN_BYTES)){

					printf("## Error: wrong file size, should be less than: %d bytes!\n", WEBFAILSAFE_UPLOAD_UBOOT_SIZE_IN_BYTES);
					webfailsafe_upload_failed = 1;

				// ART
				} else if((webfailsafe_upgrade_type == WEBFAILSAFE_UPGRADE_TYPE_ART) && (hs->upload_total != WEBFAILSAFE_UPLOAD_ART_SIZE_IN_BYTES)){

					printf("## Error: wrong file size, should be: %d bytes!\n", WEBFAILSAFE_UPLOAD_ART_SIZE_IN_BYTES);
					webfailsafe_upload_failed = 1;

				/* firmware 防变砖: 内核分区起址 = CFG_KERN_ADDR, 可用空间 =
				 * 实测 flash 总容量 - 内核分区起始偏移。超出即拒收 ——
				 * raspi_erase_write 只做"减去保留区"的总量校验, 防不住
				 * 恰好等于总容量但越过后部保留区边界的组合。按实测芯片
				 * 容量(spi_chip_info->sector_size*n_sectors)判断, 8MB/16MB
				 * 芯片自动各得其所。 */
				} else if(webfailsafe_upgrade_type == WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE){

					unsigned int kern_area = spi_chip_info->sector_size * spi_chip_info->n_sectors
								- (WEBFAILSAFE_UPLOAD_KERNEL_ADDRESS - CFG_FLASH_BASE);

					if(hs->upload_total > kern_area){
						printf("## Error: firmware too big! %d > %d (flash %dMB - reserved %dKB)\n",
							hs->upload_total, kern_area,
							(spi_chip_info->sector_size * spi_chip_info->n_sectors) >> 20,
							(WEBFAILSAFE_UPLOAD_KERNEL_ADDRESS - CFG_FLASH_BASE) >> 10);
						webfailsafe_upload_failed = 1;
					}
				}

				printf("Loading: ");

				// how much data we are storing now?
				hs->upload = (unsigned int)(uip_len - (end - (char *)uip_appdata));

				/* 上传开始前喂狗: 等待浏览器阶段(故意不喂)可能已耗掉
				 * 部分看门狗预算, 上传本身也可达 1~2 分钟, 与
				 * "upload done" 处的 kick 两头夹住 */
				wdt_gpio_kick();
				printf("## wdt: kick before upload\n");

				memcpy((void *)webfailsafe_data_pointer, (void *)end, hs->upload);
				webfailsafe_data_pointer += hs->upload;

				/* 固件数据已开始入库: 通知主循环 RUN 灯快闪 */
				webfailsafe_uploading = 1;

				httpd_download_progress();

				return(1);

			}

		} else {
			printf("## Error: couldn't find start of data!\n");
		}

	}

	return(0);
}

// called for http server app
void httpd_appcall(void){
	struct fs_file fsfile;
	unsigned int i;

	switch(uip_conn->lport){

		case HTONS(80):

			// app state
			hs = (struct httpd_state *)(uip_conn->appstate);

			// closed connection
			if(uip_closed()){
				httpd_state_reset();
				uip_close();
				return;
			}

			// aborted connection or time out occured
			if(uip_aborted() || uip_timedout()){
				httpd_state_reset();
				uip_abort();
				return;
			}

			// if we are pooled
			if(uip_poll()){
				if(hs->count++ >= 100){
					httpd_state_reset();
					uip_abort();
				}
				return;
			}

			// new connection
			if(uip_connected()){
				httpd_state_reset();
				return;
			}

			// new data in STATE_NONE
			if(uip_newdata() && hs->state == STATE_NONE){

				// GET or POST request?
				if(uip_appdata[0] == ISO_G && uip_appdata[1] == ISO_E && uip_appdata[2] == ISO_T && (uip_appdata[3] == ISO_space || uip_appdata[3] == ISO_tab)){
					hs->state = STATE_FILE_REQUEST;
				} else if(uip_appdata[0] == ISO_P && uip_appdata[1] == ISO_O && uip_appdata[2] == ISO_S && uip_appdata[3] == ISO_T && (uip_appdata[4] == ISO_space || uip_appdata[4] == ISO_tab)){
					hs->state = STATE_UPLOAD_REQUEST;
				}

				// anything else -> abort the connection!
				if(hs->state == STATE_NONE){
					httpd_state_reset();
					uip_abort();
					return;
				}

				// get file or firmware upload?
				if(hs->state == STATE_FILE_REQUEST){

					// we are looking for GET file name
					for(i = 4; i < 30; i++){
						if(uip_appdata[i] == ISO_space || uip_appdata[i] == ISO_cr || uip_appdata[i] == ISO_nl || uip_appdata[i] == ISO_tab){
							uip_appdata[i] = 0;
							i = 0;
							break;
						}
					}

					if(i != 0){
						printf("## Error: request file name too long!\n");
						httpd_state_reset();
						uip_abort();
						return;
					}

					printf("Request for: ");
					printf("%s\n", &uip_appdata[4]);

					// request for /
				if(uip_appdata[4] == ISO_slash && uip_appdata[5] == 0){
					fs_open(file_index_html.name, &fsfile);
					/* 设备信息页: flash 原文只读, 用打补丁的堆副本 */
					if (devinfo_index)
						fsfile.data = devinfo_index;
				} else {
					// check if we have requested file
					if(!fs_open((const char *)&uip_appdata[4], &fsfile)){
						printf("## Error: file not found!\n");
						fs_open(file_404_html.name, &fsfile);
					} else if (devinfo_uboot
						   && strncmp((const char *)&uip_appdata[4], "/uboot.html", 11) == 0) {
						/* 设备信息页: flash 原文只读, 用打补丁的堆副本 */
						fsfile.data = devinfo_uboot;
					}
				}

					hs->state = STATE_FILE_REQUEST;
					hs->dataptr = (u8_t *)fsfile.data;
					hs->upload = fsfile.len;

					// send first (and maybe the last) chunk of data
					uip_send(hs->dataptr, (hs->upload > uip_mss() ? uip_mss() : hs->upload));
					return;

				} else if(hs->state == STATE_UPLOAD_REQUEST){

					char *start = NULL;
					char *end = NULL;

					// end bufor data with NULL
					uip_appdata[uip_len] = '\0';

					/*
					 * We got first packet with POST request
					 *
					 * Some browsers don't include first chunk of data in the first
					 * POST request packet (like Google Chrome, IE and Safari)!
					 * So we must now find two values:
					 * - Content-Length
					 * - boundary
					 * Headers with these values can be in any order!
					 * If we don't find these values in first packet, connection will be aborted!
					 *
					 */

					// Content-Length pos
					start = (char *)strstr((char*)uip_appdata, "Content-Length:");

					if(start){

						start += sizeof("Content-Length:");

						// find end of the line with "Content-Length:"
						end = (char *)strstr(start, eol);

						if(end){

							hs->upload_total = atoi(start);
#ifdef DEBUG_UIP
							printf("Expecting %d bytes in body request message\n", hs->upload_total);
#endif

						} else {
							printf("## Error: couldn't find \"Content-Length\"!\n");
							httpd_state_reset();
							uip_abort();
							return;
						}

					} else {
						printf("## Error: couldn't find \"Content-Length\"!\n");
						httpd_state_reset();
						uip_abort();
						return;
					}

					// we don't support very small files (< 10 KB)
					if(hs->upload_total < 10240){
						printf("## Error: request for upload < 10 KB data!\n");
						httpd_state_reset();
						uip_abort();
						return;
					}

					// boundary value
					start = NULL;
					end = NULL;

					start = (char *)strstr((char *)uip_appdata, "boundary=");

					if(start){

						// move pointer over "boundary="
						start += 9;

						// find end of line with boundary value
						end = (char *)strstr((char *)start, eol);

						if(end){

							// malloc space for boundary value + '--' and '\0'
							boundary_value = (char*)malloc(end - start + 3);

							if(boundary_value){

								memcpy(&boundary_value[2], start, end - start);

								// add -- at the begin and 0 at the end
								boundary_value[0] = '-';
								boundary_value[1] = '-';
								boundary_value[end - start + 2] = 0;

#ifdef DEBUG_UIP
								printf("Found boundary value: \"%s\"\n", boundary_value);
#endif

							} else {
								printf("## Error: couldn't allocate memory for boundary!\n");
								httpd_state_reset();
								uip_abort();
								return;
							}

						} else {
							printf("## Error: couldn't find boundary!\n");
							httpd_state_reset();
							uip_abort();
							return;
						}

					} else {
						printf("## Error: couldn't find boundary!\n");
						httpd_state_reset();
						uip_abort();
						return;
					}

					/*
					 * OK, if we are here, it means that we found
					 * Content-Length and boundary values in headers
					 *
					 * We can now try to 'allocate memory' and
					 * find beginning of the data in first packet
					 */

					webfailsafe_data_pointer = (u8_t *)WEBFAILSAFE_UPLOAD_RAM_ADDRESS;

					if(!webfailsafe_data_pointer){
						printf("## Error: couldn't allocate RAM for data!\n");
						httpd_state_reset();
						uip_abort();
						return;
					} else {
						printf("Data will be downloaded at 0x%X in RAM\n", WEBFAILSAFE_UPLOAD_RAM_ADDRESS);
					}

					if(httpd_findandstore_firstchunk()){
						data_start_found = 1;
					} else {
						data_start_found = 0;
					}

					return;

				} /* else if(hs->state == STATE_UPLOAD_REQUEST) */

			} /* uip_newdata() && hs->state == STATE_NONE */

			// if we got ACK from remote host
			if(uip_acked()){

				// if we are in STATE_FILE_REQUEST state
				if(hs->state == STATE_FILE_REQUEST){

					// data which we send last time was received (we got ACK)
					// if we send everything last time -> gently close the connection
					if(hs->upload <= uip_mss()){

						// post upload completed?
						if(webfailsafe_post_done){

							if(!webfailsafe_upload_failed){
								webfailsafe_ready_for_upgrade = 1;
							}

							webfailsafe_post_done = 0;
							webfailsafe_upload_failed = 0;
						}

						httpd_state_reset();
						uip_close();
						return;
					}

					// otherwise, send another chunk of data
					// last time we sent uip_conn->len size of data
					hs->dataptr += uip_conn->len;
					hs->upload -= uip_conn->len;

					uip_send(hs->dataptr, (hs->upload > uip_mss() ? uip_mss() : hs->upload));
				}

				return;

			}

			// if we need to retransmit
			if(uip_rexmit()){

				// if we are in STATE_FILE_REQUEST state
				if(hs->state == STATE_FILE_REQUEST){
					// send again chunk of data without changing pointer and length of data left to send
					uip_send(hs->dataptr, (hs->upload > uip_mss() ? uip_mss() : hs->upload));
				}

				return;

			}

			// if we got new data frome remote host
			if(uip_newdata()){

				// if we are in STATE_UPLOAD_REQUEST state
				if(hs->state == STATE_UPLOAD_REQUEST){

					// end bufor data with NULL
					uip_appdata[uip_len] = '\0';

					// do we have to find start of data?
					if(!data_start_found){

						if(!httpd_findandstore_firstchunk()){
							printf("## Error: couldn't find start of data in next packet!\n");
							httpd_state_reset();
							uip_abort();
							return;
						} else {
							data_start_found = 1;
						}

						return;
					}

					hs->upload += (unsigned int)uip_len;

					if(!webfailsafe_upload_failed){
						memcpy((void *)webfailsafe_data_pointer, (void *)uip_appdata, uip_len);
						webfailsafe_data_pointer += uip_len;
					}

					httpd_download_progress();

					// if we have collected all data
					if(hs->upload >= hs->upload_total){

						printf("\n\n");

						// end of post upload
						webfailsafe_post_done = 1;
						webfailsafe_post_tick = get_timer(0);	/* 收尾超时兜底的起点 */
						NetBootFileXferSize = (ulong)hs->upload_total;

						// which website will be returned
						if(!webfailsafe_upload_failed){
							fs_open(file_flashing_html.name, &fsfile);
						} else {
							fs_open(file_fail_html.name, &fsfile);
						}

						httpd_state_reset();

						hs->state = STATE_FILE_REQUEST;
						hs->dataptr = (u8_t *)fsfile.data;
						hs->upload = fsfile.len;

						uip_send(hs->dataptr, (hs->upload > uip_mss() ? uip_mss() : hs->upload));
					}

				}

				return;
			}

			break;

		default:
			// we shouldn't get here... we are listening only on port 80
			uip_abort();
			break;
	}
}
