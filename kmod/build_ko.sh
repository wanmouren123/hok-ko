#!/bin/bash
# build_ko.sh — 在任意 Linux 上编译 hok_mem.ko (6.1 GKI)
# 用法: bash build_ko.sh [输出目录]
set -e

OUT=${1:-$PWD/out}
WORK=${WORK:-$PWD/.kbuild}
JOBS=$(nproc 2>/dev/null || echo 4)

echo "[*] 输出目录: $OUT"
mkdir -p "$OUT" "$WORK"

# ---- 1) 拉 GKI android14-6.1 源码(浅克隆, 约 300MB) ----
if [ ! -d "$WORK/gki" ]; then
  echo "[*] 克隆 GKI android14-6.1 ..."
  git clone --depth 1 -b android14-6.1 \
      https://android.googlesource.com/kernel/common "$WORK/gki"
fi

cd "$WORK/gki"

# ---- 2) 配置 + modules_prepare ----
if [ ! -f .config ]; then
  echo "[*] gki_defconfig ..."
  make ARCH=arm64 LLVM=0 CROSS_COMPILE=${CROSS_COMPILE:-aarch64-linux-gnu-} gki_defconfig
fi

if [ ! -f Module.symvers ] || [ ! -d include/config ]; then
  echo "[*] modules_prepare (首次约 5-15 分钟) ..."
  make ARCH=arm64 CROSS_COMPILE=${CROSS_COMPILE:-aarch64-linux-gnu-} \
       LLVM=0 modules_prepare -j"$JOBS"
fi

# ---- 3) 编我们的模块 ----
SRC=$(cd "$(dirname "$0")" && pwd)
echo "[*] 编译 hok_mem.ko ..."
make -C "$WORK/gki" M="$SRC" ARCH=arm64 \
     CROSS_COMPILE=${CROSS_COMPILE:-aarch64-linux-gnu-} \
     LLVM=0 modules -j"$JOBS"

cp "$SRC"/*.ko "$OUT"/ 2>/dev/null || true
echo "[OK] 产物:"
ls -l "$OUT"/*.ko
echo
echo "推到设备:"
echo "  adb push $OUT/hok_mem.ko /data/local/tmp/"
echo "  adb shell su -c 'insmod /data/local/tmp/hok_mem.ko'"
echo "或走 KPatch:"
echo "  adb shell su -c '/data/adb/modules/KPatch-Next/bin/kpatch kpm load /data/local/tmp/hok_mem.ko'"
