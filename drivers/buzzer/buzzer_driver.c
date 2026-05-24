#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/cdev.h>

#define DRIVER_NAME "my_buzzer"
#define CLASS_NAME  "bbb_buzzer_class"

/* ================== ĐỊA CHỈ THANH GHI BỘ CẤP XUNG NHỊP (CM_PER) ================== */
#define CM_PER_BASE               0x44E00000 
#define CM_PER_EPWMSS1_CLKCTRL    0xCC       // Cấp clock cho Subsystem PWM 1

/* ================== ĐỊA CHỈ THANH GHI PINMUX (CONTROL MODULE) ================== */
#define CTRL_MODULE_BASE          0x44E10000
#define CONF_GPMC_A2              0x848      // Offset cho chân P9_14
#define PWMSS_CTRL                0x664      // BẬT TIMEBASE CLOCK CHO PWM

/* ================== ĐỊA CHỈ THANH GHI PWM SUBSYSTEM & EHRPWM ================== */
#define PWMSS1_BASE               0x48302000 // Base PWM Subsystem 1
#define PWMSS_CLKCONFIG           0x08       // Bật clock nội bộ của PWMSS

#define EHRPWM1_BASE              0x48302200 // Base của module EHRPWM1
#define TBCTL                     0x00       // Time-Base Control
#define TBPRD                     0x0A       // Time-Base Period (Chu kỳ)
#define CMPA                      0x12       // Counter-Compare A (Độ rộng xung/Duty cycle)
#define AQCTLA                    0x16       // Action-Qualifier Control A

static dev_t dev_num;
static struct class *dev_class;
static struct cdev my_cdev;

static void __iomem *cm_per_virtual;
static void __iomem *ctrl_mod_virtual;
static void __iomem *pwmss1_virtual;
static void __iomem *ehrpwm1_virtual;

/* Hàm điều khiển âm thanh còi */
static void buzzer_set_tone(int mode) {
    uint16_t period = 0; // Khai báo an toàn cho thanh ghi 16-bit
    
    // Hệ thống sau khi qua Prescaler /8 sẽ chạy clock 12.5MHz (12.500.000 Hz)
    if (mode == 1) {
        // Lệnh '1': PASS -> Âm cao (2000Hz): 12,500,000 / 2000 = 6250
        period = 6250;  
    } else if (mode == 0) {
        // Lệnh '0': FAIL -> Âm trầm (400Hz): 12,500,000 / 400 = 31250
        period = 31250;  
    } else if (mode == 3) {
        // Lệnh '3': SẴN SÀNG (Ting) -> Âm rất thanh, trong trẻo (3000Hz): 12,500,000 / 3000 = 4166
        period = 4166;
    } else if (mode == 4) {
        // Lệnh '4': RESET (Bíp) -> Âm thanh tiêu chuẩn (1000Hz): 12,500,000 / 1000 = 12500
        period = 12500;
    } else {
        // Lệnh '2': TẮT CÒI
        writew(0, ehrpwm1_virtual + CMPA);
        return;
    }

    // Ghi chu kỳ (Period)
    writew(period, ehrpwm1_virtual + TBPRD);
    // Ghi Duty Cycle = 50% (Âm thanh to nhất)
    writew(period / 2, ehrpwm1_virtual + CMPA);
}

static int my_open(struct inode *inode, struct file *file) { return 0; }

static int my_release(struct inode *inode, struct file *file) {
    buzzer_set_tone(2); 
    return 0;
}

static ssize_t my_write(struct file *file, const char __user *user_buf, size_t size, loff_t *offset) {
    char cmd;
    if (copy_from_user(&cmd, user_buf, 1)) return -EFAULT;

    if (cmd == '1') buzzer_set_tone(1);      // PASS
    else if (cmd == '0') buzzer_set_tone(0); // FAIL
    else if (cmd == '2') buzzer_set_tone(2); // Tắt
    else if (cmd == '3') buzzer_set_tone(3); // Ting (Sẵn sàng)
    else if (cmd == '4') buzzer_set_tone(4); // Bíp (Về 0g)
    
    return size;
}

static struct file_operations fops = {
    .owner   = THIS_MODULE,
    .open    = my_open,
    .release = my_release,
    .write   = my_write, 
};

static int __init buzzer_driver_init(void) {
    uint32_t reg_val;

    // 1. Khởi tạo Device Node
    alloc_chrdev_region(&dev_num, 0, 1, DRIVER_NAME);
    dev_class = class_create(CLASS_NAME);
    cdev_init(&my_cdev, &fops);
    cdev_add(&my_cdev, dev_num, 1);
    device_create(dev_class, NULL, dev_num, NULL, DRIVER_NAME);

    // 2. Map bộ nhớ
    cm_per_virtual   = ioremap(CM_PER_BASE, 0x4000);
    ctrl_mod_virtual = ioremap(CTRL_MODULE_BASE, 0x2000);
    pwmss1_virtual   = ioremap(PWMSS1_BASE, 0x100);
    ehrpwm1_virtual  = ioremap(EHRPWM1_BASE, 0x100);

    // 3. Cấp Clock cho module PWMSS1
    writel(0x02, cm_per_virtual + CM_PER_EPWMSS1_CLKCTRL);
    while ((readl(cm_per_virtual + CM_PER_EPWMSS1_CLKCTRL) & (0x3 << 16)) != 0) {}

    // 4. Pinmux & Timebase
    writel(0x06, ctrl_mod_virtual + CONF_GPMC_A2);
    reg_val = readl(ctrl_mod_virtual + PWMSS_CTRL);
    reg_val |= (1 << 1); 
    writel(reg_val, ctrl_mod_virtual + PWMSS_CTRL);

    // 5. Bật Clock nội bộ của PWMSS
    reg_val = readl(pwmss1_virtual + PWMSS_CLKCONFIG);
    reg_val |= (1 << 8); 
    writel(reg_val, pwmss1_virtual + PWMSS_CLKCONFIG);

    // ==========================================================
    // 6. FIX LỖI TIMEBASE CONTROL (TBCTL) - Bật Bộ chia tần (Prescaler)
    // ==========================================================
    // Cấu hình: CLKDIV = /8 (0x3 << 10), HSPCLKDIV = /1 (0x0), Up-count mode (0x0)
    // Giá trị cần ghi = 0x0C00. Điều này giúp xung nhịp hạ từ 100MHz xuống 12.5MHz.
    writew(0x0C00, ehrpwm1_virtual + TBCTL);
    // ==========================================================
    
    // AQCTLA: Kéo HIGH khi Counter = 0, kéo LOW khi Counter = CMPA
    writew(0x0012, ehrpwm1_virtual + AQCTLA);

    buzzer_set_tone(2);

    pr_info("Buzzer PWM Driver: Khoi tao thanh cong tren chan P9_14 (Voi Prescaler /8)\n");
    return 0;
}

static void __exit buzzer_driver_exit(void) {
    buzzer_set_tone(2); 

    iounmap(ehrpwm1_virtual);
    iounmap(pwmss1_virtual);
    iounmap(ctrl_mod_virtual);
    iounmap(cm_per_virtual);

    device_destroy(dev_class, dev_num);
    cdev_del(&my_cdev);
    class_destroy(dev_class);
    unregister_chrdev_region(dev_num, 1);
    
    pr_info("Buzzer PWM Driver: Da go bo\n");
}

module_init(buzzer_driver_init);
module_exit(buzzer_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ban");
MODULE_DESCRIPTION("Register-level EHRPWM Driver for Buzzer - Fix Overflow");
