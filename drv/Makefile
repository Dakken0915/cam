obj-m += lcd_drv.o

KDIR ?= /home/hxh1987370733/toolchain/100ask_imx6ull-sdk/Linux-4.9.88
ARCH ?= arm
SDK_ROOT ?= /home/hxh1987370733/toolchain/100ask_imx6ull-sdk
CROSS_COMPILE ?= $(SDK_ROOT)/ToolChain/gcc-linaro-6.2.1-2016.11-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean
