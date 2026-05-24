#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>

#define DS3231_ADDR 0x68
#define DRIVER_NAME "ds3231_driver"

static struct i2c_client *ds3231_client;

// Hàm chuyển đổi từ BCD sang Decimal
static int bcd2dec(u8 bcd) {
    return ((bcd & 0xF0) >> 4) * 10 + (bcd & 0x0F);
}

// Hàm chuyển đổi từ Decimal sang BCD
static u8 dec2bcd(int dec) {
    return ((dec / 10) << 4) | (dec % 10);
}

// Hàm đọc thời gian từ DS3231
static ssize_t ds3231_read(struct file *file, char __user *user_buf, size_t count, loff_t *ppos) {
    u8 regs[7];
    char time_str[32];
    int ret, len;

    if (*ppos > 0) return 0; // Chỉ đọc 1 lần

    // Đọc 7 thanh ghi bắt đầu từ 0x00 (Giây, Phút, Giờ, Thứ, Ngày, Tháng, Năm)
    ret = i2c_smbus_read_i2c_block_data(ds3231_client, 0x00, 7, regs);
    if (ret < 0) {
        pr_err("Lỗi đọc dữ liệu từ DS3231\n");
        return -EIO;
    }

    // Parse dữ liệu (Mask bit theo datasheet DS3231)
    int sec = bcd2dec(regs[0]);
    int min = bcd2dec(regs[1]);
    int hour = bcd2dec(regs[2] & 0x3F); // Chế độ 24h
    int mday = bcd2dec(regs[4]);
    int mon = bcd2dec(regs[5] & 0x7F);
    int year = bcd2dec(regs[6]) + 2000;

    // Format chuỗi trả về: "YYYY-MM-DD HH:MM:SS"
    len = snprintf(time_str, sizeof(time_str), "%04d-%02d-%02d %02d:%02d:%02d\n", 
                   year, mon, mday, hour, min, sec);

    if (count < len) return -EINVAL;

    if (copy_to_user(user_buf, time_str, len)) {
        return -EFAULT;
    }

    *ppos = len;
    return len;
}

// Hàm ghi thời gian vào DS3231 (Cập nhật RTC)
static ssize_t ds3231_write(struct file *file, const char __user *user_buf, size_t count, loff_t *ppos) {
    char buf[32];
    int year, mon, mday, hour, min, sec;
    u8 data[8];
    int ret;

    if (count > 31) count = 31;
    if (copy_from_user(buf, user_buf, count)) return -EFAULT;
    buf[count] = '\0';

    // Parse chuỗi YYYY-MM-DD HH:MM:SS
    if (sscanf(buf, "%d-%d-%d %d:%d:%d", &year, &mon, &mday, &hour, &min, &sec) != 6) {
        pr_err("Sai định dạng thời gian. Vui lòng dùng: YYYY-MM-DD HH:MM:SS\n");
        return -EINVAL;
    }

    if (year >= 2000) year -= 2000; // Chỉ lưu 2 số cuối của năm

    data[0] = dec2bcd(sec);
    data[1] = dec2bcd(min);
    data[2] = dec2bcd(hour); // Chế độ 24h
    data[3] = 1; // Thứ (Day of week 1-7), để mặc định là 1
    data[4] = dec2bcd(mday);
    data[5] = dec2bcd(mon);
    data[6] = dec2bcd(year);

    ret = i2c_smbus_write_i2c_block_data(ds3231_client, 0x00, 7, data);
    if (ret < 0) {
        pr_err("Lỗi khi ghi thời gian xuống DS3231\n");
        return -EIO;
    }

    return count;
}

// Cấu trúc thao tác file
static const struct file_operations ds3231_fops = {
    .owner = THIS_MODULE,
    .read = ds3231_read,
    .write = ds3231_write,
};

// Cấu trúc device node
static struct miscdevice ds3231_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "myds3231",
    .fops = &ds3231_fops,
};

static int ds3231_probe(struct i2c_client *client, const struct i2c_device_id *id) {
    int ret;
    
    ds3231_client = client;

    // Kích hoạt dao động (Oscillator)
    i2c_smbus_write_byte_data(client, 0x0E, 0x00); // Control register: Enable Oscillator (clear ~EOSC bit)
    i2c_smbus_write_byte_data(client, 0x0F, 0x00); // Status register: Clear OSF (Oscillator Stop Flag)

    // Đăng ký file /dev/myds3231
    ret = misc_register(&ds3231_misc);
    if (ret) {
        pr_err("Lỗi tạo device node cho DS3231\n");
        return ret;
    }

    pr_info("Đã nhận dạng DS3231 tại I2C address 0x%x\n", client->addr);
    pr_info("Đã tạo node /dev/myds3231\n");
    return 0;
}

// Hàm chạy khi gỡ driver
static int ds3231_remove(struct i2c_client *client) {
    misc_deregister(&ds3231_misc);
    pr_info("Đã gỡ driver DS3231\n");
    return 0;
}

// Khai báo Device Tree match table
static const struct of_device_id ds3231_of_match[] = {
    { .compatible = "my,ds3231", },
    { }
};
MODULE_DEVICE_TABLE(of, ds3231_of_match);

static struct i2c_driver ds3231_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = ds3231_of_match,
    },
    .probe = ds3231_probe,
    .remove = ds3231_remove,
};

module_i2c_driver(ds3231_driver);

MODULE_AUTHOR("Antigravity AI");
MODULE_DESCRIPTION("Custom I2C Driver for DS3231 RTC");
MODULE_LICENSE("GPL");
