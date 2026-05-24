#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>

#define DRIVER_NAME "sh1106_spi_driver"

static struct spi_device *sh1106_spi;
static struct gpio_desc *dc_gpio;
static struct gpio_desc *reset_gpio;

// Hàm gửi 1 byte qua SPI
static int sh1106_spi_send(u8 data, bool is_cmd) {
    if (dc_gpio) gpiod_set_value(dc_gpio, is_cmd ? 0 : 1);
    return spi_write(sh1106_spi, &data, 1);
}

// Hàm gửi mảng byte qua SPI (dữ liệu)
static int sh1106_spi_send_data_buffer(const u8 *data, size_t len) {
    if (dc_gpio) gpiod_set_value(dc_gpio, 1); // Data mode
    return spi_write(sh1106_spi, data, len);
}

// Hàm gửi lệnh (Command)
static void sh1106_write_cmd(u8 cmd) {
    sh1106_spi_send(cmd, true);
}

// Khởi tạo SH1106
static void sh1106_init(void) {
    if (reset_gpio) {
        gpiod_set_value(reset_gpio, 0);
        msleep(50);
        gpiod_set_value(reset_gpio, 1);
        msleep(50);
    }

    sh1106_write_cmd(0xAE); // Display OFF
    sh1106_write_cmd(0xD5); // Set Display Clock Divide Ratio
    sh1106_write_cmd(0x80); 
    sh1106_write_cmd(0xA8); // Set Multiplex Ratio
    sh1106_write_cmd(0x3F); 
    sh1106_write_cmd(0xD3); // Set Display Offset
    sh1106_write_cmd(0x00); 
    sh1106_write_cmd(0x40); // Set Display Start Line
    sh1106_write_cmd(0xAD); // DC-DC Control Mode Set
    sh1106_write_cmd(0x8B); // DC-DC ON
    sh1106_write_cmd(0xA1); // Segment Re-map
    sh1106_write_cmd(0xC8); // COM Output Scan Direction
    sh1106_write_cmd(0xDA); // Set COM Pins Hardware Configuration
    sh1106_write_cmd(0x12); 
    sh1106_write_cmd(0x81); // Set Contrast Control
    sh1106_write_cmd(0x80); 
    sh1106_write_cmd(0xD9); // Set Pre-charge Period
    sh1106_write_cmd(0x1F); 
    sh1106_write_cmd(0xDB); // Set VCOMH Deselect Level
    sh1106_write_cmd(0x40); 
    sh1106_write_cmd(0x33); // Set VPP
    sh1106_write_cmd(0xA6); // Normal Display
    sh1106_write_cmd(0xA4); // Entire Display ON
    sh1106_write_cmd(0xAF); // Display ON
}

// Hàm ghi dữ liệu (Framebuffer 1024 bytes) từ User Space
static ssize_t sh1106_write(struct file *file, const char __user *user_buf, size_t count, loff_t *ppos) {
    u8 *buffer;
    int page, i;

    if (count != 1024) {
        pr_err("SH1106 SPI: Kích thước dữ liệu phải là 1024 bytes\n");
        return -EINVAL;
    }

    buffer = kmalloc(1024, GFP_KERNEL);
    if (!buffer) return -ENOMEM;

    if (copy_from_user(buffer, user_buf, 1024)) {
        kfree(buffer);
        return -EFAULT;
    }

    for (page = 0; page < 8; page++) {
        sh1106_write_cmd(0xB0 + page); // Set Page Address
        sh1106_write_cmd(0x02);        // Set Lower Column Address (SH1106 offset is 2)
        sh1106_write_cmd(0x10);        // Set Higher Column Address
        
        // Gửi 128 bytes dữ liệu cho trang này
        sh1106_spi_send_data_buffer(&buffer[page * 128], 128);
    }

    kfree(buffer);
    return count;
}

static const struct file_operations sh1106_fops = {
    .owner = THIS_MODULE,
    .write = sh1106_write,
};

static struct miscdevice sh1106_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "myoled",
    .fops = &sh1106_fops,
};

static int sh1106_spi_probe(struct spi_device *spi) {
    int ret;
    sh1106_spi = spi;

    // Thiết lập cấu hình SPI
    spi->bits_per_word = 8;
    spi_setup(spi);

    // Lấy GPIO từ Device Tree
    dc_gpio = gpiod_get(&spi->dev, "dc", GPIOD_OUT_LOW);
    if (IS_ERR(dc_gpio)) {
        pr_err("Không thể lấy chân DC GPIO\n");
        return PTR_ERR(dc_gpio);
    }

    reset_gpio = gpiod_get_optional(&spi->dev, "reset", GPIOD_OUT_HIGH);
    if (IS_ERR(reset_gpio)) {
        pr_err("Không thể lấy chân Reset GPIO\n");
        gpiod_put(dc_gpio);
        return PTR_ERR(reset_gpio);
    }

    sh1106_init();

    ret = misc_register(&sh1106_misc);
    if (ret) {
        pr_err("Lỗi tạo device node cho SH1106 SPI\n");
        if (reset_gpio) gpiod_put(reset_gpio);
        gpiod_put(dc_gpio);
        return ret;
    }

    pr_info("Đã nhận dạng SH1106 SPI\n");
    pr_info("Đã tạo node /dev/myoled\n");
    return 0;
}

static int sh1106_spi_remove(struct spi_device *spi) {
    sh1106_write_cmd(0xAE); // Display OFF
    misc_deregister(&sh1106_misc);
    if (reset_gpio) gpiod_put(reset_gpio);
    if (dc_gpio) gpiod_put(dc_gpio);
    pr_info("Đã gỡ driver SH1106 SPI\n");
    return 0;
}

static const struct of_device_id sh1106_spi_of_match[] = {
    { .compatible = "my,sh1106-spi", },
    { }
};
MODULE_DEVICE_TABLE(of, sh1106_spi_of_match);

static struct spi_driver sh1106_spi_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = sh1106_spi_of_match,
    },
    .probe = sh1106_spi_probe,
    .remove = sh1106_spi_remove,
};

module_spi_driver(sh1106_spi_driver);

MODULE_AUTHOR("Antigravity AI");
MODULE_DESCRIPTION("Custom SPI Driver for SH1106 OLED Display");
MODULE_LICENSE("GPL");
