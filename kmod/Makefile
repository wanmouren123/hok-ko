# hok_mem.ko — 王者荣耀透视模块 内核部分
# 用法:
#   export KDIR=/path/to/kernel/(已编译的目标内核源码树 或 GKI kernel-headers)
#   export CROSS=aarch64-linux-gnu-
#   make
#
# GKI 设备: 从 https://github.com/aosp-mirror/kernel_common 拉对应分支,
#           make O=out ARCH=arm64 gki_defconfig && make O=out modules_prepare

obj-m += hok_mem.o
ccflags-y += -O2 -fno-stack-protector -w

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=arm64 CROSS_COMPILE=$(CROSS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
