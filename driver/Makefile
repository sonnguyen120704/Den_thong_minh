obj-m += pir_driver.o pwm_driver.o bh1750_driver.o ds3231_driver.o sh1106_spi_driver.o

BUILDROOT_DIR ?= /home/son/buildroot2

# Đường dẫn trỏ tới Kernel Source trong Buildroot của bạn
KDIR ?= $(BUILDROOT_DIR)/output/build/linux-custom

# Trình biên dịch Cross-compiler của Buildroot
CROSS_COMPILE ?= $(BUILDROOT_DIR)/output/host/bin/arm-buildroot-linux-gnueabihf-

all:
	make -C $(KDIR) M=$(PWD) ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	make -C $(KDIR) M=$(PWD) ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) clean
