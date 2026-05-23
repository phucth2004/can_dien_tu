obj-m += lcd_i2c.o

# Đường dẫn Kernel của Buildroot
KDIR := /home/truong/buildroot/output/build/linux-custom

# Đường dẫn Toolchain của Buildroot
CROSS := /home/truong/buildroot/output/host/bin/arm-buildroot-linux-gnueabihf-

PWD := $(shell pwd)

all:
	$(MAKE) ARCH=arm CROSS_COMPILE=$(CROSS) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) ARCH=arm CROSS_COMPILE=$(CROSS) -C $(KDIR) M=$(PWD) clean
