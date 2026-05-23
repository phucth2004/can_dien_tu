obj-m += tcrt5000_driver.o

# Đường dẫn tới thư mục mã nguồn Kernel
KDIR := /home/truong/buildroot/output/build/linux-custom

# Kiến trúc ARM
ARCH := arm

# Trỏ chính xác vào bộ công cụ biên dịch (Toolchain) mà Buildroot đã sinh ra
CROSS_COMPILE := /home/truong/buildroot/output/host/bin/arm-buildroot-linux-gnueabihf-

all:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(PWD) clean
