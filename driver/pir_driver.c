#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/device.h>

#define DRIVER_NAME "pir_driver"
#define DRIVER_CLASS "pir_class"
#define PIR_GPIO 60  // Chân P9_12 tương ứng GPIO 60

static int major_number;
static struct class *pir_class = NULL;
static struct device *pir_device = NULL;

static unsigned int pir_irq_number;
static bool motion_detected = false;

// -----------------------------------------------------------
// Ngắt cứng (Interrupt Service Routine - ISR)
// Hàm này sẽ tự động chạy ngay lập tức khi chân tín hiệu thay đổi
// -----------------------------------------------------------
static irqreturn_t pir_isr(int irq, void *dev_id) {
    int state = gpio_get_value(PIR_GPIO);
    motion_detected = (state == 1);
    
    // In log ra hệ thống (dùng lệnh dmesg để xem)
    printk(KERN_INFO "PIR: Motion state changed: %d\n", motion_detected);
    
    return IRQ_HANDLED;
}

// -----------------------------------------------------------
// Hàm giao tiếp với Userspace (khi ứng dụng C dùng lệnh read())
// -----------------------------------------------------------
static ssize_t pir_read(struct file *filep, char __user *buffer, size_t len, loff_t *offset) {
    char msg[2];
    int bytes_to_copy;
    
    // Nếu ứng dụng đã đọc xong thì dừng (tránh vòng lặp vô hạn)
    if (*offset > 0) return 0; 
    
    // msg[0] sẽ là '1' nếu có chuyển động, '0' nếu không
    msg[0] = motion_detected ? '1' : '0';
    msg[1] = '\n';
    bytes_to_copy = 2;
    
    if (len < bytes_to_copy) return -EINVAL;
    
    // Copy biến từ không gian Kernel ra không gian Userspace
    if (copy_to_user(buffer, msg, bytes_to_copy)) {
        return -EFAULT;
    }
    
    *offset += bytes_to_copy;
    return bytes_to_copy;
}

static struct file_operations fops = {
    .read = pir_read,
    .owner = THIS_MODULE,
};

// -----------------------------------------------------------
// Hàm khởi tạo (Chạy khi gõ lệnh insmod pir_driver.ko)
// -----------------------------------------------------------
static int __init pir_init(void) {
    int ret;

    printk(KERN_INFO "PIR: Initializing the PIR Driver\n");

    // 1. Xin hệ điều hành cấp phát một Major Number tự động
    major_number = register_chrdev(0, DRIVER_NAME, &fops);
    if (major_number < 0) {
        printk(KERN_ALERT "PIR: Failed to register a major number\n");
        return major_number;
    }

    // 2. Tạo thư mục class trong /sys/class/
    pir_class = class_create(THIS_MODULE, DRIVER_CLASS);
    if (IS_ERR(pir_class)) {
        unregister_chrdev(major_number, DRIVER_NAME);
        return PTR_ERR(pir_class);
    }

    // 3. Tạo file thiết bị vật lý /dev/mypir (để Userspace gọi vào)
    pir_device = device_create(pir_class, NULL, MKDEV(major_number, 0), NULL, "mypir");
    if (IS_ERR(pir_device)) {
        class_destroy(pir_class);
        unregister_chrdev(major_number, DRIVER_NAME);
        return PTR_ERR(pir_device);
    }

    // 4. Khởi tạo chân GPIO phần cứng
    if (!gpio_is_valid(PIR_GPIO)) {
        printk(KERN_ALERT "PIR: Invalid GPIO %d\n", PIR_GPIO);
        goto r_device;
    }

    gpio_request(PIR_GPIO, "sysfs");
    gpio_direction_input(PIR_GPIO);

    // 5. Móc ngắt (Interrupt Hooking) vào chân GPIO này
    pir_irq_number = gpio_to_irq(PIR_GPIO);
    ret = request_irq(pir_irq_number,
                      pir_isr,
                      IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, // Ngắt cả lúc tay đưa vào và tay rút ra
                      "pir_handler",
                      NULL);
                      
    if (ret) {
        printk(KERN_ALERT "PIR: Failed to request IRQ %d\n", pir_irq_number);
        gpio_free(PIR_GPIO);
        goto r_device;
    }

    printk(KERN_INFO "PIR: Driver initialized! Device node /dev/mypir created.\n");
    return 0;

r_device:
    device_destroy(pir_class, MKDEV(major_number, 0));
    class_destroy(pir_class);
    unregister_chrdev(major_number, DRIVER_NAME);
    return -ENODEV;
}

// -----------------------------------------------------------
// Hàm dọn dẹp (Chạy khi gõ lệnh rmmod pir_driver)
// -----------------------------------------------------------
static void __exit pir_exit(void) {
    free_irq(pir_irq_number, NULL);              // Giải phóng ngắt
    gpio_free(PIR_GPIO);                         // Giải phóng GPIO
    device_destroy(pir_class, MKDEV(major_number, 0)); // Xóa /dev/mypir
    class_destroy(pir_class);
    unregister_chrdev(major_number, DRIVER_NAME);
    
    printk(KERN_INFO "PIR: Driver unloaded successfully.\n");
}

module_init(pir_init);
module_exit(pir_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Son");
MODULE_DESCRIPTION("A Linux Character Device Driver with Interrupts for PIR SR602");
MODULE_VERSION("1.0");
