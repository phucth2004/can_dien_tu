/*
 * hx711.c - Kernel driver đọc loadcell HX711F qua GPIO bit-banging
 *           BeagleBone Black (AM335x), thao tác trực tiếp thanh ghi GPIO
 *
 * Các lỗi đã sửa so với bản gốc:
 *
 *   [BUG #1/#9] hx711_read(): Đọc DOUT đúng thời điểm theo HX711F datasheet.
 *               Bản gốc đọc DOUT khi SCK đang HIGH → bắt được trạng thái
 *               TRƯỚC khi HX711 kịp shift bit ra → data sai hoàn toàn.
 *               Thứ tự đúng theo datasheet Figure 2:
 *                 1. SCK lên HIGH  (rising edge — HX711 shift bit ra DOUT)
 *                 2. Chờ T2 ≥ 0.1µs
 *                 3. ĐỌC DOUT      ← điểm đọc đúng
 *                 4. SCK xuống LOW (falling edge)
 *                 5. Chờ T4 ≥ 0.2µs (SCK low time)
 *
 *   [BUG #2]  Timeout chờ DOUT: tăng từ 10ms lên 200ms.
 *             HX711 10Hz = 1 sample/100ms. Timeout 10ms luôn hết hạn
 *             trước khi chip kịp chuyển đổi xong → ETIMEDOUT liên tục.
 *
 *   [BUG #3]  local_irq_save() chỉ bao quanh phần bit-bang (48µs),
 *             không bao gồm vòng chờ DOUT (có thể dài tới 100ms).
 *             Giữ ngắt tắt 100ms làm treo toàn kernel.
 *
 *   [WARN #4] Kiểm tra count < sizeof(int32_t) trước copy_to_user.
 *
 *   [WARN #5] Reset *pos = 0 sau mỗi read() để sensor_node có thể
 *             gọi read() liên tiếp mà không bị kernel trả về 0.
 *
 *   [WARN #6] Thêm class_create + device_create để /dev/hx711 xuất hiện
 *             tự động sau insmod, không cần mknod thủ công.
 *
 *   [WARN #7] Chờ IDLEST=FUNC sau khi bật clock GPIO1.
 *
 *   [WARN #8] Không tắt clock GPIO1 trong exit() vì có thể có driver
 *             khác đang dùng GPIO1 cùng lúc (LED, button, v.v.).
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/cdev.h>
#include <linux/device.h>

#define DRIVER_NAME "hx711"
#define CLASS_NAME  "hx711_class"

/* ======================================================================
 * Clock Manager Peripheral (CM_PER)
 * ====================================================================== */
#define CM_PER_BASE             0x44E00000
#define CM_PER_GPIO1_CLKCTRL    0xAC

#define CLKCTRL_MODULEMODE_ENABLE   0x02
#define CLKCTRL_IDLEST_MASK         (0x3 << 16)
#define CLKCTRL_IDLEST_FUNC         (0x0 << 16)

/* ======================================================================
 * GPIO1 (0x4804C000)
 * Chân dùng:
 *   DOUT → GPIO1_28 → Header P9_12
 *   SCK  → GPIO1_16 → Header P9_15
 * ====================================================================== */
#define GPIO1_BASE          0x4804C000
#define GPIO_OE             0x134   /* Output Enable: 1=input, 0=output */
#define GPIO_DATAIN         0x138   /* Đọc trạng thái chân               */
#define GPIO_CLEARDATAOUT   0x190   /* Kéo chân xuống LOW                */
#define GPIO_SETDATAOUT     0x194   /* Kéo chân lên HIGH                 */

#define DOUT_PIN    28  /* P9_12 — input  */
#define SCK_PIN     16  /* P9_15 — output */

/*
 * Timeout chờ DOUT xuống LOW.
 * HX711 10Hz → 1 sample mỗi 100ms.
 * Đặt 200 vòng × 1ms = 200ms để có đủ thời gian chờ 2 sample liên tiếp.
 */
#define DOUT_TIMEOUT_MS     200

/* ======================================================================
 * Biến toàn cục
 * ====================================================================== */
static dev_t            dev_num;
static struct class    *dev_class;
static struct cdev      hx711_cdev;
static void __iomem    *cm_per_virtual;
static void __iomem    *gpio_base_virtual;

/* ======================================================================
 * File operations
 * ====================================================================== */

static int hx711_open(struct inode *inode, struct file *file)
{
    pr_info("HX711: Device opened\n");
    return 0;
}

static int hx711_release(struct inode *inode, struct file *file)
{
    pr_info("HX711: Device closed\n");
    return 0;
}

/*
 * hx711_read() - Đọc 1 giá trị 24-bit từ HX711F qua GPIO bit-banging.
 *
 * Timing chuẩn theo HX711F datasheet v2.0, Figure 2:
 *
 *   DOUT  ‾‾‾‾‾|_______________|‾ (high khi chưa sẵn sàng, low khi sẵn)
 *   SCK   _____|‾|_|‾|_|‾|_|...   (25 xung)
 *
 *   Mỗi bit:
 *     T3: SCK HIGH ≥ 0.2µs (dùng udelay(1) = 1µs — đủ)
 *     T2: Sau rising edge, HX711 đẩy bit ra DOUT trong 0.1µs
 *         → đọc DOUT sau udelay(1) là an toàn
 *     T4: SCK LOW ≥ 0.2µs (dùng udelay(1) = 1µs — đủ)
 *
 *   Xung thứ 25: thiết lập gain=128, Channel A cho lần đọc tiếp theo.
 */
static ssize_t hx711_read(struct file *file, char __user *user_buf,
                          size_t count, loff_t *pos)
{
    int32_t       raw = 0;
    int           i;
    int           timeout = DOUT_TIMEOUT_MS;
    unsigned long irq_flags;

    /* [WARN #4 FIX] Kiểm tra buffer đủ lớn */
    if (count < sizeof(int32_t))
        return -EINVAL;

    /*
     * Chờ DOUT xuống LOW — chip sẵn sàng trả data.
     * [BUG #2 FIX] Timeout tăng lên 200ms (bản gốc: 10ms).
     * [BUG #3 FIX] Vòng chờ này NGOÀI critical section ngắt.
     *              Không được tắt ngắt ở đây vì có thể chờ tới 100ms.
     */
    while (readl(gpio_base_virtual + GPIO_DATAIN) & BIT(DOUT_PIN)) {
        usleep_range(1000, 1100);   /* 1ms mỗi vòng */
        if (--timeout == 0) {
            pr_err("HX711: DOUT timeout — kiem tra ket noi SCK/DOUT\n");
            return -ETIMEDOUT;
        }
    }

    /*
     * [BUG #3 FIX] Tắt ngắt CHỈ bao quanh phần bit-bang (≈48µs).
     * Kernel cho phép tắt ngắt tối đa ~100µs trong driver production.
     * 24 bit × (1µs HIGH + 1µs LOW) = 48µs — nằm trong giới hạn.
     */
    local_irq_save(irq_flags);

    for (i = 0; i < 24; i++) {
        raw <<= 1;

        /* Bước 1: SCK lên HIGH — HX711 bắt đầu shift bit ra DOUT */
        writel(BIT(SCK_PIN), gpio_base_virtual + GPIO_SETDATAOUT);
        udelay(1);  /* T3: SCK high time ≥ 0.2µs → dùng 1µs */

        /*
         * [BUG #1/#9 FIX] Đọc DOUT SAU rising edge, khi SCK vẫn HIGH.
         * Bản gốc đọc DOUT rồi mới tạo rising edge → đọc bit cũ.
         * Theo datasheet T2: sau rising edge ≤ 0.1µs HX711 đã đẩy bit
         * ra DOUT. udelay(1) = 1µs >> 0.1µs → đủ thời gian settle.
         */
        if (readl(gpio_base_virtual + GPIO_DATAIN) & BIT(DOUT_PIN))
            raw |= 1;

        /* Bước 2: SCK xuống LOW */
        writel(BIT(SCK_PIN), gpio_base_virtual + GPIO_CLEARDATAOUT);
        udelay(1);  /* T4: SCK low time ≥ 0.2µs → dùng 1µs */
    }

    /*
     * Xung thứ 25: thiết lập Channel A, Gain = 128 cho lần đọc tiếp.
     * Theo datasheet Table 3: 25 pulses → CH.A, Gain 128.
     */
    writel(BIT(SCK_PIN), gpio_base_virtual + GPIO_SETDATAOUT);
    udelay(1);
    writel(BIT(SCK_PIN), gpio_base_virtual + GPIO_CLEARDATAOUT);
    udelay(1);

    local_irq_restore(irq_flags);

    /*
     * Chuyển đổi từ offset binary sang 2's complement có dấu.
     * HX711F output: 0x800000 = MIN (âm nhất), 0x7FFFFF = MAX (dương nhất)
     * XOR 0x800000 để đưa về signed int32 chuẩn.
     * (Xem reference driver trong datasheet: Count=Count^0x800000)
     */
    raw ^= 0x800000;

    /* Copy 4 byte giá trị signed int32 lên user-space */
    if (copy_to_user(user_buf, &raw, sizeof(raw)))
        return -EFAULT;

    /*
     * [WARN #5 FIX] Reset offset về 0.
     * Nếu không, kernel VFS coi đã đọc đến EOF và từ chối read() tiếp theo.
     * sensor_node gọi read() 10 lần/giây → bắt buộc phải reset.
     */
    *pos = 0;

    return sizeof(raw);
}

static const struct file_operations fops = {
    .owner   = THIS_MODULE,
    .open    = hx711_open,
    .read    = hx711_read,
    .release = hx711_release,
};

/* ======================================================================
 * Module init / exit
 * ====================================================================== */

static int __init hx711_init(void)
{
    int      ret;
    int      wait;
    uint32_t reg_oe;

    pr_info("HX711: Initializing register-level GPIO driver...\n");

    /* ----------------------------------------------------------------
     * 1. Cấp phát character device
     * [WARN #6 FIX] Dùng alloc_chrdev_region + class + device
     * ---------------------------------------------------------------- */
    ret = alloc_chrdev_region(&dev_num, 0, 1, DRIVER_NAME);
    if (ret < 0) {
        pr_err("HX711: alloc_chrdev_region failed: %d\n", ret);
        return ret;
    }

    dev_class = class_create(CLASS_NAME);
    if (IS_ERR(dev_class)) {
        ret = PTR_ERR(dev_class);
        pr_err("HX711: class_create failed: %d\n", ret);
        goto err_unreg;
    }

    cdev_init(&hx711_cdev, &fops);
    ret = cdev_add(&hx711_cdev, dev_num, 1);
    if (ret < 0) {
        pr_err("HX711: cdev_add failed: %d\n", ret);
        goto err_class;
    }

    if (IS_ERR(device_create(dev_class, NULL, dev_num, NULL, DRIVER_NAME))) {
        pr_err("HX711: device_create failed\n");
        ret = -ENODEV;
        goto err_cdev;
    }

    /* ----------------------------------------------------------------
     * 2. Map và bật clock GPIO1
     * ---------------------------------------------------------------- */
    cm_per_virtual = ioremap(CM_PER_BASE, 0x4000);
    if (!cm_per_virtual) {
        pr_err("HX711: ioremap CM_PER failed\n");
        ret = -EIO;
        goto err_device;
    }

    writel(CLKCTRL_MODULEMODE_ENABLE,
           cm_per_virtual + CM_PER_GPIO1_CLKCTRL);

    /* [WARN #7 FIX] Chờ IDLEST = FUNC trước khi truy cập GPIO */
    wait = 1000;
    while ((readl(cm_per_virtual + CM_PER_GPIO1_CLKCTRL) & CLKCTRL_IDLEST_MASK)
           != CLKCTRL_IDLEST_FUNC) {
        if (--wait == 0) {
            pr_warn("HX711: GPIO1 clock may not be stable\n");
            break;
        }
        cpu_relax();
    }

    /* ----------------------------------------------------------------
     * 3. Map GPIO1 và cấu hình chân
     * ---------------------------------------------------------------- */
    gpio_base_virtual = ioremap(GPIO1_BASE, 0x1000);
    if (!gpio_base_virtual) {
        pr_err("HX711: ioremap GPIO1 failed\n");
        ret = -EIO;
        goto err_iounmap_clk;
    }

    /* Đọc OE hiện tại, set DOUT=input (bit=1), SCK=output (bit=0) */
    reg_oe  = readl(gpio_base_virtual + GPIO_OE);
    reg_oe |=  BIT(DOUT_PIN);   /* input  */
    reg_oe &= ~BIT(SCK_PIN);    /* output */
    writel(reg_oe, gpio_base_virtual + GPIO_OE);

    /* Khởi tạo SCK = LOW (chip ở trạng thái normal operation) */
    writel(BIT(SCK_PIN), gpio_base_virtual + GPIO_CLEARDATAOUT);

    pr_info("HX711: /dev/%s ready (DOUT=GPIO1_%d P9_12, SCK=GPIO1_%d P9_15)\n",
            DRIVER_NAME, DOUT_PIN, SCK_PIN);
    return 0;

    /* Error paths */
err_iounmap_clk:
    iounmap(cm_per_virtual);
    cm_per_virtual = NULL;
err_device:
    device_destroy(dev_class, dev_num);
err_cdev:
    cdev_del(&hx711_cdev);
err_class:
    class_destroy(dev_class);
err_unreg:
    unregister_chrdev_region(dev_num, 1);
    return ret;
}

static void __exit hx711_exit(void)
{
    if (gpio_base_virtual) {
        /* SCK = LOW trước khi tháo driver (chip về normal mode) */
        writel(BIT(SCK_PIN), gpio_base_virtual + GPIO_CLEARDATAOUT);
        iounmap(gpio_base_virtual);
        gpio_base_virtual = NULL;
    }

    /*
     * [WARN #8 FIX] KHÔNG tắt clock GPIO1 ở đây.
     * GPIO1 có thể đang được dùng bởi các driver khác (LED, button...).
     * Chỉ iounmap để giải phóng virtual mapping.
     */
    if (cm_per_virtual) {
        iounmap(cm_per_virtual);
        cm_per_virtual = NULL;
    }

    device_destroy(dev_class, dev_num);
    cdev_del(&hx711_cdev);
    class_destroy(dev_class);
    unregister_chrdev_region(dev_num, 1);

    pr_info("HX711: Driver removed\n");
}

module_init(hx711_init);
module_exit(hx711_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nhom 9 - HDHN");
MODULE_DESCRIPTION("Register-level GPIO Driver for HX711F on BeagleBone Black - Fixed");
MODULE_VERSION("2.0");
