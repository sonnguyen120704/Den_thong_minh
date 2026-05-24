#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/device.h>

#define DRIVER_NAME "bh1750_driver"
#define DRIVER_CLASS "bh1750_class"

static struct class *bh1750_class;
static struct device *bh1750_device;
static struct i2c_client *bh1750_client;
static int major_number;

// Lệnh I2C của BH1750
#define CMD_POWER_ON        0x01
#define CMD_POWER_DOWN      0x00
#define CMD_ONE_TIME_H_RES  0x20

// -----------------------------------------------------------
// Hàm xử lý khi Userspace gọi lệnh read() trên /dev/bh1750
// -----------------------------------------------------------
static ssize_t bh1750_read(struct file *filep, char __user *buffer, size_t len, loff_t *offset) {
    int ret;
    u8 buf[2];
    int lux;
    char out_str[16];
    int out_len;
    
    // Nếu ứng dụng C đã đọc xong thì dừng
    if (*offset > 0) return 0;
    
    if (!bh1750_client) return -ENODEV;

    // 1. Gửi lệnh đo ánh sáng 1 lần ở độ phân giải cao
    ret = i2c_smbus_write_byte(bh1750_client, CMD_ONE_TIME_H_RES);
    if (ret < 0) {
        printk(KERN_ERR "BH1750: Failed to send measurement command\n");
        return ret;
    }
    
    // 2. Chờ 180ms theo yêu cầu của datasheet để chip đo đạc
    msleep(180);
    
    // 3. Đọc 2 byte dữ liệu kết quả từ I2C bus
    ret = i2c_master_recv(bh1750_client, buf, 2);
    if (ret < 0) {
        printk(KERN_ERR "BH1750: Failed to read data\n");
        return ret;
    }
    
    // 4. Tính toán Lux ngay trong Kernel (Công thức: (Byte_cao * 256 + Byte_thấp) / 1.2)
    lux = ((buf[0] << 8) | buf[1]) * 10 / 12; // Nhân 10 chia 12 để tránh số thập phân float trong Kernel
    
    // 5. Trả kết quả dạng chuỗi về Userspace
    out_len = snprintf(out_str, sizeof(out_str), "%d\n", lux);
    if (len < out_len) return -EINVAL;
    
    if (copy_to_user(buffer, out_str, out_len)) return -EFAULT;
    
    *offset += out_len;
    return out_len;
}

static struct file_operations fops = {
    .read = bh1750_read,
    .owner = THIS_MODULE,
};

// -----------------------------------------------------------
// Hàm Probe (Chạy khi Kernel phát hiện có thiết bị I2C tương thích trong DTS)
// -----------------------------------------------------------
static int bh1750_probe(struct i2c_client *client, const struct i2c_device_id *id) {
    bh1750_client = client;
    
    printk(KERN_INFO "BH1750: Probing device at address 0x%02x...\n", client->addr);
    
    // Khởi tạo Character Device
    major_number = register_chrdev(0, DRIVER_NAME, &fops);
    if (major_number < 0) return major_number;
    
    bh1750_class = class_create(THIS_MODULE, DRIVER_CLASS);
    if (IS_ERR(bh1750_class)) {
        unregister_chrdev(major_number, DRIVER_NAME);
        return PTR_ERR(bh1750_class);
    }
    
    bh1750_device = device_create(bh1750_class, NULL, MKDEV(major_number, 0), NULL, "bh1750");
    if (IS_ERR(bh1750_device)) {
        class_destroy(bh1750_class);
        unregister_chrdev(major_number, DRIVER_NAME);
        return PTR_ERR(bh1750_device);
    }
    
    // Bật nguồn cảm biến
    i2c_smbus_write_byte(client, CMD_POWER_ON);
    
    printk(KERN_INFO "BH1750: Probed successfully! Device node /dev/bh1750 created.\n");
    return 0;
}

static int bh1750_remove(struct i2c_client *client) {
    // Tắt nguồn để tiết kiệm điện
    i2c_smbus_write_byte(client, CMD_POWER_DOWN);
    
    device_destroy(bh1750_class, MKDEV(major_number, 0));
    class_destroy(bh1750_class);
    unregister_chrdev(major_number, DRIVER_NAME);
    
    printk(KERN_INFO "BH1750: Driver removed\n");
    return 0;
}

// Danh sách thiết bị (ID) mà driver này hỗ trợ
static const struct i2c_device_id bh1750_id[] = {
    { "bh1750", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, bh1750_id);

// Bảng Match với Device Tree (DTS)
static const struct of_device_id bh1750_of_match[] = {
    { .compatible = "rohm,bh1750", },
    { }
};
MODULE_DEVICE_TABLE(of, bh1750_of_match);

// Khai báo cấu trúc I2C Driver chuẩn của Linux
static struct i2c_driver bh1750_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = bh1750_of_match,
    },
    .probe = bh1750_probe,
    .remove = bh1750_remove,
    .id_table = bh1750_id,
};

module_i2c_driver(bh1750_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Son");
MODULE_DESCRIPTION("I2C Client Driver for BH1750 Light Sensor");
