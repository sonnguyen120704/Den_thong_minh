#include <linux/module.h>
#include <linux/pwm.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/err.h>

#define PERIOD_NS 1000000 // Tần số 1kHz (1 chu kỳ = 1ms = 1,000,000 ns)

static struct pwm_device *pwm_dev;
static struct class *led_class;
static struct device *led_device;
static int current_brightness = 0; // Độ sáng từ 0 đến 100

// -----------------------------------------------------------
// Hàm xử lý khi Userspace đọc file /sys/class/smart_lamp/led/brightness
// -----------------------------------------------------------
static ssize_t brightness_show(struct device *dev, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%d\n", current_brightness);
}

// -----------------------------------------------------------
// Hàm xử lý khi Userspace ghi vào /sys/class/smart_lamp/led/brightness
// -----------------------------------------------------------
static ssize_t brightness_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
    long val;
    long duty_ns;
    
    // Chuyển chuỗi từ Userspace thành số nguyên
    if (kstrtol(buf, 10, &val) < 0) return -EINVAL;
    
    // Giới hạn giá trị 0 - 100
    if (val < 0) val = 0;
    if (val > 100) val = 100;

    current_brightness = val;
    duty_ns = (val * PERIOD_NS) / 100; // Tính toán ra Nano-giây ngay trong Kernel
    
    // Áp dụng cấu hình cho phần cứng PWM
    pwm_config(pwm_dev, duty_ns, PERIOD_NS);
    
    if (val > 0) {
        pwm_enable(pwm_dev);
    } else {
        pwm_disable(pwm_dev);
    }
    
    return count;
}

// Macro tự động tạo ra file 'brightness' có quyền Read/Write
static DEVICE_ATTR_RW(brightness);

// -----------------------------------------------------------
// Hàm Probe (Được gọi tự động khi Kernel tìm thấy Device Tree tương ứng)
// -----------------------------------------------------------
static int smart_led_probe(struct platform_device *pdev) {
    int ret;
    
    printk(KERN_INFO "PWM_LED: Probing Smart LED Device...\n");
    
    // Lấy đối tượng PWM từ Device Tree (tự động mapping tới chân P9_22)
    pwm_dev = devm_pwm_get(&pdev->dev, NULL);
    if (IS_ERR(pwm_dev)) {
        printk(KERN_ERR "PWM_LED: Could not get PWM device from DTS\n");
        return PTR_ERR(pwm_dev);
    }

    // Tắt đèn lúc ban đầu
    pwm_config(pwm_dev, 0, PERIOD_NS);
    pwm_disable(pwm_dev);

    // Tạo thư mục /sys/class/smart_lamp
    led_class = class_create(THIS_MODULE, "smart_lamp");
    if (IS_ERR(led_class)) return PTR_ERR(led_class);

    // Tạo thư mục con /sys/class/smart_lamp/led
    led_device = device_create(led_class, NULL, MKDEV(0,0), NULL, "led");
    if (IS_ERR(led_device)) {
        class_destroy(led_class);
        return PTR_ERR(led_device);
    }

    // Đính kèm file 'brightness' vào thư mục led
    ret = device_create_file(led_device, &dev_attr_brightness);
    if (ret) {
        device_destroy(led_class, MKDEV(0,0));
        class_destroy(led_class);
        return ret;
    }

    printk(KERN_INFO "PWM_LED: Driver probed successfully! Created /sys/class/smart_lamp/led/brightness\n");
    return 0;
}

// -----------------------------------------------------------
// Hàm Remove (Được gọi khi rmmod hoặc rút thiết bị)
// -----------------------------------------------------------
static int smart_led_remove(struct platform_device *pdev) {
    pwm_disable(pwm_dev);
    device_remove_file(led_device, &dev_attr_brightness);
    device_destroy(led_class, MKDEV(0,0));
    class_destroy(led_class);
    printk(KERN_INFO "PWM_LED: Driver removed\n");
    return 0;
}

// Khai báo bảng Match với Device Tree
// Chữ "son,smart-led" này phải khớp 100% với tên trong file .dts
static const struct of_device_id smart_led_of_match[] = {
    { .compatible = "son,smart-led", },
    { },
};
MODULE_DEVICE_TABLE(of, smart_led_of_match);

static struct platform_driver smart_led_driver = {
    .driver = {
        .name = "smart_led_driver",
        .of_match_table = smart_led_of_match,
    },
    .probe = smart_led_probe,
    .remove = smart_led_remove,
};

module_platform_driver(smart_led_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Son");
MODULE_DESCRIPTION("Platform Driver for PWM LED using Device Tree");
