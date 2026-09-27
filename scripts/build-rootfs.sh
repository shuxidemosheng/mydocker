#!/bin/bash
# scripts/build-rootfs.sh —— 构建最小 busybox rootfs
# 用法: ./scripts/build-rootfs.sh   （在仓库根目录执行，无需 root）
# 原理: busybox 是把常用 Unix 工具集成的单个静态二进制，
#       /bin 下的一堆符号链接只是让它"扮演"不同命令（busybox 装聪明）。
set -euo pipefail
cd "$(dirname "$0")/.."

# 1. 免 root 获取 busybox：apt-get download 只下载 .deb 不安装，dpkg -x 直接解开
if [ ! -x rootfs/bin/busybox ]; then
    mkdir -p .debs rootfs
    if [ ! -x .debs/extract/usr/bin/busybox ]; then
        ( cd .debs
          apt-get download busybox-static
          dpkg -x busybox-static*.deb extract )
    fi
    cp .debs/extract/usr/bin/busybox rootfs/bin/busybox
fi

# 2. FHS 标准目录骨架：容器里即使是 busybox 世界也要有这些空目录
mkdir -p rootfs/{bin,sbin,usr/bin,usr/sbin,proc,sys,dev,tmp,etc,root,home}
chmod 1777 rootfs/tmp                      # /tmp 需要 sticky bit，和真系统一致

# 3. 生成常用命令的符号链接
cd rootfs/bin
for t in sh ash bash ls cat echo pwd mount umount ps hostname sleep \
         mkdir rmdir rm cp mv touch ln grep head tail wc id uname \
         ip ping cat chmod chown kill; do
    [ -e "$t" ] || ln -sf busybox "$t"
done
cd ../..

echo "rootfs 就绪: $(ls rootfs/bin | wc -l) 个命令链接"
rootfs/bin/sh -c 'echo ROOTFS_SH_OK'
