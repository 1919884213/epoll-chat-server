# 对着目标内核源码树编译（板子或 VM 侧，Windows 上编不出 aarch64 模块）：
#
#   make KDIR=~/linux-kernel/linux-6.1 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
#
# 板子上本地编译（板子已有内核头）：
#
#   make            # 默认 KDIR=/lib/modules/$(uname -r)/build
#
obj-m += chatsnap.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
