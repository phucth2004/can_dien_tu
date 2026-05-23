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
    uint32_t period = 0;
    
    // Hệ thống chạy clock 100MHz (Chu kỳ = 100.000.000 / Tần số)
    if (mode == 1) {
        // Lệnh '1': PASS -> Âm cao (2000Hz)
        period = 50000; 
    } else if (mode == 0) {
        // Lệnh '0': FAIL -> Âm trầm (400Hz)
        period = 250000; 
    } else if (mode == 3) {
        // Lệnh '3': SẴN SÀNG (Ting) -> Âm rất thanh, trong trẻo (3000Hz)
        period = 33333;
    } else if (mode == 4) {
        // Lệnh '4': RESET (Bíp) -> Âm thanh tiêu chuẩn (1000Hz)
        period = 100000;
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

static int my_open(struct inode *inode, struct file *file) {
    return 0;
}

static int my_release(struct inode *inode, struct file *file) {
    buzzer_set_tone(2); // Tự động tắt còi khi đóng file
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

    // 1. Khởi tạo Device Node tự động
    alloc_chrdev_region(&dev_num, 0, 1, DRIVER_NAME);
    dev_class = class_create(CLASS_NAME);
    cdev_init(&my_cdev, &fops);
    cdev_add(&my_cdev, dev_num, 1);
    device_create(dev_class, NULL, dev_num, NULL, DRIVER_NAME);

    // 2. Map bộ nhớ vật lý
    cm_per_virtual   = ioremap(CM_PER_BASE, 0x4000);
    ctrl_mod_virtual = ioremap(CTRL_MODULE_BASE, 0x2000);
    pwmss1_virtual   = ioremap(PWMSS1_BASE, 0x100);
    ehrpwm1_virtual  = ioremap(EHRPWM1_BASE, 0x100);

    // 3. Cấp Clock cho module PWMSS1 (Ghi 0x02)
    writel(0x02, cm_per_virtual + CM_PER_EPWMSS1_CLKCTRL);

    // [QUAN TRỌNG - FIX LỖI KERNEL OOPS] 
    // Chờ cho đến khi bit 16, 17 (IDLEST) về 0 (Module hoàn toàn tỉnh dậy)
    while ((readl(cm_per_virtual + CM_PER_EPWMSS1_CLKCTRL) & (0x3 << 16)) != 0) {
        // Không làm gì cả, chỉ chờ phần cứng sẵn sàng
    }

    // 4. Pinmux: Ép chân P9_14 thành Mode 6 (EHRPWM1A)
    writel(0x06, ctrl_mod_virtual + CONF_GPMC_A2);

    // 4.5. Bật Time-Base Clock cho PWMSS1 trong Control Module (Rất hay quên trên BBB)
    // Bit 0: PWMSS0, Bit 1: PWMSS1, Bit 2: PWMSS2
    reg_val = readl(ctrl_mod_virtual + PWMSS_CTRL);
    reg_val |= (1 << 1); 
    writel(reg_val, ctrl_mod_virtual + PWMSS_CTRL);

    // 5. Lúc này bộ PWM đã thức hoàn toàn, có thể truy cập an toàn mà không bị Oops
    reg_val = readl(pwmss1_virtual + PWMSS_CLKCONFIG);
    reg_val |= (1 << 8); 
    writel(reg_val, pwmss1_virtual + PWMSS_CLKCONFIG);

    // 6. Cấu hình thanh ghi EHRPWM để phát xung
    // TBCTL: Prescaler = 1 (Tốc độ tối đa), Up-count mode (0x00)
    writew(0x0000, ehrpwm1_virtual + TBCTL);
    
    // AQCTLA: Kéo HIGH khi Counter = 0, kéo LOW khi Counter = CMPA
    // Đạt 0 thì Set (bit 0-1 = 0x2), Đạt CMPA thì Clear (bit 4-5 = 0x1) -> 0x0012
    writew(0x0012, ehrpwm1_virtual + AQCTLA);

    // Khởi tạo ban đầu: Tắt còi
    buzzer_set_tone(2);

    pr_info("Buzzer PWM Driver: Khoi tao thanh cong tren chan P9_14\n");
    return 0;
}

static void __exit buzzer_driver_exit(void) {
    buzzer_set_tone(2); // Tắt còi

    // Hủy map bộ nhớ
    iounmap(ehrpwm1_virtual);
    iounmap(pwmss1_virtual);
    iounmap(ctrl_mod_virtual);
    iounmap(cm_per_virtual);

    // Hủy Device
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
MODULE_DESCRIPTION("Register-level EHRPWM Driver for Buzzer");
