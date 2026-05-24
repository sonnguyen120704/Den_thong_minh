obj-m += pir_driver.o pwm_driver.o bh1750_driver.o ds3231_driver.o sh1106_spi_driver.o

# Đường dẫn trỏ tới Kernel Source trong Buildroot của bạn
KDIR := /home/son/buildroot2/output/build/linux-custom

# Trình biên dịch Cross-compiler của Buildroot
CROSS_COMPILE := /home/son/buildroot2/output/host/bin/arm-buildroot-linux-gnueabihf-

all:
	make -C $(KDIR) M=$(PWD) ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	make -C $(KDIR) M=$(PWD) ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) clean
