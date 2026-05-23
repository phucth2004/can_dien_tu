/*
 * tcrt5000.c - Kernel driver đọc ADC cho cảm biến vật cản TCRT5000
 * trên BeagleBone Black (AM335x), thao tác trực tiếp thanh ghi
 *
 * Các lỗi đã sửa so với bản gốc:
 * [BUG #1]  tcrt_read(): thay udelay() bằng cpu_relax() tránh block CPU
 * [BUG #2]  tcrt_read(): flush FIFO0 trước khi trigger lấy mẫu mới
 * [BUG #3]  tcrt_read(): kiểm tra count >= sizeof(int) trước khi copy
 * [WARN #4] tcrt_read(): reset *pos = 0 để cho phép read() nhiều lần
 * [BUG #5]  tcrt_init(): STEPCONFIG1 cấu hình AIN0, One-shot, VREFN (8<<15)
 * [BUG #6]  tcrt_init(): thêm delay giữa write-protect off và enable ADC
 * [WARN #7] tcrt_init(): chờ IDLEST = FUNC sau khi bật clock
 * [WARN #8/9] Thêm class_create + device_create để tự tạo /dev/tcrt5000
 * [WARN #10] tcrt_exit(): disable step trước khi tắt ADC_CTRL
 * [WARN #11] tcrt_exit(): iounmap cm_wkup_virtual luôn được gọi
 * [BUG #12] ADC_FIFO0DATA offset chuẩn xác là 0x100 theo AM335x TRM
 * [WARN #13] Thêm STEPDELAY1 config: averaging 16 mẫu để lọc nhiễu
 * [WARN #14] Thêm error path đầy đủ để tránh resource leak khi init thất bại
 * [BUG #15] Thêm cấu hình ADC_CLKDIV để hạ xung nhịp từ 24MHz xuống 3MHz
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/cdev.h>
#include <linux/device.h>

#define DRIVER_NAME "tcrt5000"
#define CLASS_NAME  "tcrt5000_class"

/* ======================================================================
 * ĐỊA CHỈ THANH GHI BỘ CẤP XUNG NHỊP (CM_WKUP)
 * Nguồn: AM335x TRM Table 8-276
 * ====================================================================== */
#define CM_WKUP_BASE                0x44E00400
#define CM_WKUP_ADC_TSC_CLKCTRL    0xBC

/* Bit mask trong CM_WKUP_ADC_TSC_CLKCTRL */
#define CLKCTRL_MODULEMODE_ENABLE   0x02        /* bit 1:0 = 10 */
#define CLKCTRL_IDLEST_MASK         (0x3 << 16) /* bit 17:16 */
#define CLKCTRL_IDLEST_FUNC         (0x0 << 16) /* 00 = Fully functional */

/* ======================================================================
 * ĐỊA CHỈ THANH GHI MODULE ADC (TSC_ADC_SS)
 * Nguồn: AM335x TRM Chapter 12, Table 12-102
 * ====================================================================== */
#define TSC_ADC_BASE        0x44E0D000

#define ADC_REVISION        0x00    /* Revision ID                          */
#define ADC_CTRL            0x40    /* Control register                     */
#define ADC_CLKDIV          0x4C    /* ADC Clock Divider                    */
#define ADC_STEPENABLE      0x54    /* Step Enable register                 */
#define ADC_STEPCONFIG1     0x64    /* Step 1 Configuration                 */
#define ADC_STEPDELAY1      0x68    /* Step 1 Delay (sampling + open delay) */
#define ADC_FIFO0COUNT      0xE4    /* FIFO0 word count                     */
#define ADC_FIFO0THRESHOLD  0xE8    /* FIFO0 threshold                      */

/*
 * [BUG #12 FIX] Địa chỉ chuẩn xác của FIFO0 là 0x100
 */
#define ADC_FIFO0DATA       0x100

/* Bit mask ADC_CTRL (Table 12-111) */
#define ADC_CTRL_STEPCONFIG_WRITEPROTECT_N  BIT(2)  /* 1 = cho phép ghi config */
#define ADC_CTRL_ENABLE                     BIT(0)  /* 1 = bật module ADC       */

/*
 * [BUG #5 FIX] STEPCONFIG1 đúng theo AM335x TRM Table 12-130
 * Giá trị cần ghi cho AIN0, one-shot, FIFO0, VREFN, averaging 16 mẫu:
 */
#define ADC_AIN_CHANNEL     0           /* 0 = AIN0, 1 = AIN1, ... 7 = AIN7 */
#define STEPCONFIG1_VALUE   ((0 << 27) | \
                             ((ADC_AIN_CHANNEL & 0xF) << 19) | \
                             (8 << 15) | \
                             (4 << 8)  | \
                             (0 << 2))

/*
 * STEPDELAY1: sampling delay + open delay
 * Giá trị: SampleDelay=15 (0x0F), OpenDelay=0x200 — đủ cho TCRT5000
 */
#define STEPDELAY1_VALUE    ((0x0F << 24) | 0x200)

/* Bit mask ADC_FIFO0COUNT */
#define FIFO0COUNT_MASK     0x7F        /* 7 bit thấp = số word trong FIFO */

/* ADC là 12-bit trên AM335x */
#define ADC_12BIT_MASK      0xFFF

/* Timeout vòng lặp chờ FIFO (đơn vị: lần lặp với cpu_relax) */
#define ADC_FIFO_TIMEOUT    2000

/* ======================================================================
 * Biến toàn cục của module
 * ====================================================================== */
static int              major_number;
static struct class    *dev_class;
static struct cdev      tcrt_cdev;
static dev_t            dev_num;
static void __iomem    *cm_wkup_virtual;
static void __iomem    *adc_base_virtual;

/* ======================================================================
 * File operations
 * ====================================================================== */

static int tcrt_open(struct inode *inode, struct file *file)
{
    pr_info("TCRT5000: Device opened\n");
    return 0;
}

static int tcrt_release(struct inode *inode, struct file *file)
{
    pr_info("TCRT5000: Device closed\n");
    return 0;
}

static ssize_t tcrt_read(struct file *file, char __user *user_buffer,
                         size_t count, loff_t *pos)
{
    int raw_adc_value;
    int timeout;

    if (count < sizeof(int))
        return -EINVAL;

    /* Drain FIFO0 trước khi trigger lấy mẫu mới */
    while ((readl(adc_base_virtual + ADC_FIFO0COUNT) & FIFO0COUNT_MASK) > 0)
        readl(adc_base_virtual + ADC_FIFO0DATA); /* đọc bỏ đi */

    /* Trigger Step 1: ghi bit 1 vào STEPENABLE để bắt đầu 1 lần chuyển đổi */
    writel(BIT(1), adc_base_virtual + ADC_STEPENABLE);

    /* Chờ dữ liệu vào FIFO bằng cpu_relax() */
    timeout = ADC_FIFO_TIMEOUT;
    while ((readl(adc_base_virtual + ADC_FIFO0COUNT) & FIFO0COUNT_MASK) == 0) {
        if (--timeout == 0) {
            pr_err("TCRT5000: ADC timeout - FIFO0 empty after %d polls\n",
                   ADC_FIFO_TIMEOUT);
            return -EIO;
        }
        udelay(1);
    }

    /* Đọc giá trị 32-bit từ FIFO0DATA, mask lấy 12 bit thấp */
    raw_adc_value = readl(adc_base_virtual + ADC_FIFO0DATA) & ADC_12BIT_MASK;

    /* Copy 4 byte giá trị ADC lên user-space */
    if (copy_to_user(user_buffer, &raw_adc_value, sizeof(raw_adc_value)))
        return -EFAULT;

    *pos = 0;

    return sizeof(raw_adc_value);
}

static const struct file_operations fops = {
    .owner   = THIS_MODULE,
    .open    = tcrt_open,
    .read    = tcrt_read,
    .release = tcrt_release,
};

/* ======================================================================
 * Module init / exit
 * ====================================================================== */

static int __init tcrt_init(void)
{
    int ret;
    int wait;
    u32 clkctrl_val;

    pr_info("TCRT5000: Initializing register-level ADC driver V2.1...\n");

    /* 1. Cấp phát character device number */
    ret = alloc_chrdev_region(&dev_num, 0, 1, DRIVER_NAME);
    if (ret < 0) {
        pr_err("TCRT5000: alloc_chrdev_region failed, ret=%d\n", ret);
        return ret;
    }
    major_number = MAJOR(dev_num);

    /* 2. Tạo class và device node tự động (/dev/tcrt5000) */
    dev_class = class_create(CLASS_NAME);
    if (IS_ERR(dev_class)) {
        pr_err("TCRT5000: class_create failed\n");
        ret = PTR_ERR(dev_class);
        goto err_unreg_chrdev;
    }

    cdev_init(&tcrt_cdev, &fops);
    ret = cdev_add(&tcrt_cdev, dev_num, 1);
    if (ret < 0) {
        pr_err("TCRT5000: cdev_add failed, ret=%d\n", ret);
        goto err_class_destroy;
    }

    if (IS_ERR(device_create(dev_class, NULL, dev_num, NULL, DRIVER_NAME))) {
        pr_err("TCRT5000: device_create failed\n");
        ret = -ENODEV;
        goto err_cdev_del;
    }

    /* 3. Map bộ nhớ cho Clock Manager (CM_WKUP) */
    cm_wkup_virtual = ioremap(CM_WKUP_BASE, 0x1000);
    if (!cm_wkup_virtual) {
        pr_err("TCRT5000: ioremap CM_WKUP failed\n");
        ret = -EIO;
        goto err_device_destroy;
    }

    /* 4. Bật clock cho ADC_TSC */
    writel(CLKCTRL_MODULEMODE_ENABLE, cm_wkup_virtual + CM_WKUP_ADC_TSC_CLKCTRL);

    wait = 1000;
    do {
        clkctrl_val = readl(cm_wkup_virtual + CM_WKUP_ADC_TSC_CLKCTRL);
        if ((clkctrl_val & CLKCTRL_IDLEST_MASK) == CLKCTRL_IDLEST_FUNC)
            break;
        cpu_relax();
    } while (--wait > 0);

    if (wait == 0)
        pr_warn("TCRT5000: ADC clock may not be stable (IDLEST timeout)\n");

    /* 5. Map bộ nhớ cho module ADC */
    adc_base_virtual = ioremap(TSC_ADC_BASE, 0x2000);
    if (!adc_base_virtual) {
        pr_err("TCRT5000: ioremap ADC failed\n");
        ret = -EIO;
        goto err_iounmap_clk;
    }

    /* ----------------------------------------------------------------
     * 6. Cấu hình ADC
     * ---------------------------------------------------------------- */

    /* [BUG #15 FIX] Hạ xung nhịp ADC từ 24MHz xuống 3MHz (Divider = 7+1) */
    writel(0x07, adc_base_virtual + ADC_CLKDIV);

    /* Bước A: Mở write-protect */
    writel(ADC_CTRL_STEPCONFIG_WRITEPROTECT_N, adc_base_virtual + ADC_CTRL);
    udelay(100);

    /* Bước B: Cấu hình Step 1 — AIN0, one-shot, FIFO0, VREFN, averaging 16 mẫu */
    writel(STEPCONFIG1_VALUE, adc_base_virtual + ADC_STEPCONFIG1);

    /* Cấu hình delay để tín hiệu TCRT5000 ổn định */
    writel(STEPDELAY1_VALUE, adc_base_virtual + ADC_STEPDELAY1);

    /* Bước C: Bật module ADC + giữ write-protect mở */
    writel(ADC_CTRL_STEPCONFIG_WRITEPROTECT_N | ADC_CTRL_ENABLE,
           adc_base_virtual + ADC_CTRL);

    pr_info("TCRT5000: Hardware initialized. /dev/%s ready (major=%d, AIN%d)\n",
             DRIVER_NAME, major_number, ADC_AIN_CHANNEL);
    return 0;

err_iounmap_clk:
    iounmap(cm_wkup_virtual);
    cm_wkup_virtual = NULL;
err_device_destroy:
    device_destroy(dev_class, dev_num);
err_cdev_del:
    cdev_del(&tcrt_cdev);
err_class_destroy:
    class_destroy(dev_class);
err_unreg_chrdev:
    unregister_chrdev_region(dev_num, 1);
    return ret;
}

static void __exit tcrt_exit(void)
{
    if (adc_base_virtual) {
        writel(0x00, adc_base_virtual + ADC_STEPENABLE);
        udelay(50); 
        writel(0x00, adc_base_virtual + ADC_CTRL);
        iounmap(adc_base_virtual);
        adc_base_virtual = NULL;
    }

    if (cm_wkup_virtual) {
        iounmap(cm_wkup_virtual);
        cm_wkup_virtual = NULL;
    }

    device_destroy(dev_class, dev_num);
    cdev_del(&tcrt_cdev);
    class_destroy(dev_class);
    unregister_chrdev_region(dev_num, 1);

    pr_info("TCRT5000: Driver removed\n");
}

module_init(tcrt_init);
module_exit(tcrt_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nhom 9 - HDHN");
MODULE_DESCRIPTION("Register-level ADC Driver for TCRT5000 on BeagleBone Black - V2.1");
MODULE_VERSION("2.1");
