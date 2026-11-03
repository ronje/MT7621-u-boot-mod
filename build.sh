#!/bin/bash
#
# AX1800 (MT7621A + DDR3 256MB + SPI NOR 32MB) 一键构建
#
# 用法:
#   ./build.sh ax1800        # 输出 uboot_ax1800_256M_4300_buildYYYYMMDDHH.bin
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

product=${1:-ax1800}

if [ "$product" != "ax1800" ]; then
    echo "[ERROR] 未知产品: $product (当前仅支持 ax1800)"
    exit 1
fi

cfg=".config_${product}_256M"
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

echo "==> will build $product (MT7621A, DDR3 256MB, SPI NOR 32MB, 880MHz)"
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
out="uboot_${product}_256M_4300_build${build_date}.bin"
echo "==> copy uboot.bin -> $out ($(stat -c %s uboot.bin) bytes)"
cp -f uboot.bin "$out"
mkdir -p /mnt/share_file/uboot
cp -f "$out" "/mnt/share_file/uboot/" 2>/dev/null && \
    echo "==> Build completed: /mnt/share_file/uboot/$out" || \
    echo "==> Build completed: $out (共享目录不可用, 产物在项目根目录)"
