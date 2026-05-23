obj-m += hx711_driver.o

# Đường dẫn chính xác tới Kernel Source đã build thành công
KDIR := /home/truong/buildroot/output/build/linux-custom

# Cấu hình biên dịch chéo cho kiến trúc ARM
ARCH := arm
CROSS_COMPILE := /home/truong/buildroot/output/host/bin/arm-buildroot-linux-gnueabihf-

all:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(PWD) clean
