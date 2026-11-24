#!/bin/bash
#
# AX1800 (MT7621A + DDR3 128MB/256MB + SPI NOR 32MB) 一键构建
#
# 用法(通用名 mt7621_xxxM 与 ax1800_xxxM 等价, 都用 AX1800 的配置, 只是产物名不同):
#   ./build.sh ax1800          # = ax1800_256M, 输出 uboot_ax1800_256M_4300_buildYYYYMMDDHH.bin
#   ./build.sh ax1800_256M     # 同上
#   ./build.sh ax1800_128M     # 输出 uboot_ax1800_128M_4300_buildYYYYMMDDHH.bin
#   ./build.sh mt7621_256M     # .config_ax1800_256M, 输出 uboot_mt7621_256M_4300_buildYYYYMMDDHH.bin
#   ./build.sh mt7621_128M     # .config_ax1800_128M, 输出 uboot_mt7621_128M_4300_buildYYYYMMDDHH.bin
#
# DDR 颗粒容量是编译期固定(ON_BOARD_xxxM_DRAM_COMPONENT + DDR_CHIP -> ddr_param 段),
# 不是自适应: 128MB 板必须烧 ax1800_128M 产物, 256MB 板必须烧 ax1800_256M 产物,
# 不能混用(stage 的 "Auto-Detection" 只自动识别 DDR2/DDR3 与做时序校准)。
#
# 流程: 预设 .config -> 自动生成 autoconf.h(免 menuconfig 交互)
#       -> make (uboot.bin = 主uboot + stage_sram + DDR参数拼装) -> 产物归档
#
# 分区布局(SPI 标准, 同 mt7628_uboot_web 语义):
#   0x000000 uboot   192KB | 0x030000 env 64KB | 0x040000 factory 64KB
#   0x050000 kernel 起, 升级上限 = 芯片容量(运行时探测, 32MB) - 320KB
#
# 升级入口(烧录后):
#   按住 reset 上电 3~8s 松手 -> httpd web 升级 (电脑 10.10.10.3 访问 10.10.10.1)
#   按住 reset 上电 >8s 松手  -> TFTP 工厂安装 (服务器 10.10.10.3, 文件
#                               mt7621-squashfs-sysupgrade.bin)
#   倒计时内输 666 -> 命令行: httpd(刷 web) / tftpflash(刷 TFTP) / tftp(只下载)

product=${1:-ax1800}

# 通用名 mt7621_xxxM 与 ax1800_xxxM 同配置; cfg 一律取 AX1800 预设, 产物名按用户输入的名
case "$product" in
    ax1800|ax1800_256M|mt7621_256M)
        size=256M
        ;;
    ax1800_128M|mt7621_128M)
        size=128M
        ;;
    *)
        echo "[ERROR] 未知产品: $product (支持 ax1800 / ax1800_256M / ax1800_128M / mt7621_256M / mt7621_128M)"
        exit 1
        ;;
esac

name=$product
[ "$name" = "ax1800" ] && name=ax1800_256M	# 无参/ax1800 沿用带容量的产物名, 与历史一致
cfg=".config_ax1800_${size}"
ddr="DDR3 ${size%%M}MB"
if [ ! -f "$cfg" ]; then
    echo "[ERROR] 配置文件不存在: $cfg"
    exit 1
fi

echo "==> 使用配置: $cfg"
cp -f "$cfg" .config

# 交叉编译器(项目内 toolchain, 免依赖 /opt):
# 注入绝对路径 —— .config 的相对路径在 make -C 子目录中会失效
TOOLCHAIN_BIN="$PWD/toolchain/mips-2012.03/bin"
if [ ! -x "$TOOLCHAIN_BIN/mipsel-linux-gcc" ]; then
    echo "[ERROR] 缺少交叉编译器: $TOOLCHAIN_BIN/mipsel-linux-gcc"
    echo "        请先解压: tar -xjf toolchain/mips-2012.03.tar.bz2 -C toolchain/"
    exit 1
fi
sed -i "s|^CONFIG_CROSS_COMPILER_PATH=.*|CONFIG_CROSS_COMPILER_PATH=\"$TOOLCHAIN_BIN\"|" .config

# 自动生成 autoconf.h (等价于 make menuconfig 的产物, 免交互):
#   XXX=y            -> #define XXX 1
#   # XXX is not set -> #undef  XXX
#   XXX=值/串        -> #define XXX 值/串
{
    echo "#define AUTOCONF_INCLUDED"
    awk '
        /^#/   { next }
        /^[ \t]*$/ { next }
        / is not set$/ { sub(/^# /, ""); printf "#undef  %s\n", $1; next }
        /=y$/ { sub(/=y$/, ""); printf "#define %s 1\n", $1; next }
        /=/   { i = index($0, "=");
                printf "#define %s %s\n", substr($0, 1, i-1), substr($0, i+1); next }
    ' .config
} > autoconf.h
echo "==> autoconf.h 生成完毕: $(wc -l < autoconf.h) 行"

echo "==> will build $name ($cfg, MT7621A, $ddr, SPI NOR 32MB, 880MHz)"
sleep 1
make clean > /dev/null 2>&1
# 注: 不用 -j 并行 —— Makefile 的 all: clean $(ALL) 在并行下 clean 会与
#     目标产物竞争(删掉刚生成的文件); 单线程顺序构建
make

if [ ! -f uboot.bin ]; then
    echo "[ERROR] 构建失败: uboot.bin 未生成"
    exit 1
fi

build_date=$(date +%Y%m%d%H)
out="uboot_${name}_4300_build${build_date}.bin"
echo "==> copy uboot.bin -> $out ($(stat -c %s uboot.bin) bytes)"
cp -f uboot.bin "$out"
mkdir -p /mnt/share_file/uboot
cp -f "$out" "/mnt/share_file/uboot/" 2>/dev/null && \
    echo "==> Build completed: /mnt/share_file/uboot/$out" || \
    echo "==> Build completed: $out (共享目录不可用, 产物在项目根目录)"
