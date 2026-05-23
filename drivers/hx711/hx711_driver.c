#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/delay.h>

#define DRIVER_NAME "hx711"

/* ================== ĐỊA CHỈ THANH GHI BỘ CẤP XUNG NHỊP (CM_PER) ================== */
#define CM_PER_BASE               0x44E00000 // Địa chỉ vật lý Base module Clock Peripheral
#define CM_PER_GPIO1_CLKCTRL      0xAC       // Offset thanh ghi Clock của GPIO1

/* ================== ĐỊA CHỈ THANH GHI MODULE GPIO1 ================== */
#define GPIO1_BASE                0x4804C000 // Địa chỉ vật lý Base module GPIO1
#define GPIO_OE                   0x134      // Offset thanh ghi cấu hình In/Out
#define GPIO_DATAIN               0x138      // Offset thanh ghi đọc dữ liệu
#define GPIO_CLEARDATAOUT         0x190      // Offset thanh ghi kéo chân xuống LOW
#define GPIO_SETDATAOUT           0x194      // Offset thanh ghi kéo chân lên HIGH

// Định nghĩa chân (Ví dụ: Header P9 - DOUT là P9_12, SCK là P9_15)
#define DOUT_PIN 28
#define SCK_PIN  16

static int major_number;
static void __iomem *cm_per_virtual;   // Con trỏ map bộ nhớ Clock
static void __iomem *gpio_base_virtual; // Con trỏ map bộ nhớ GPIO

/* Hàm được gọi khi User-Space dùng lệnh open() */
static int hx711_open(struct inode *inode, struct file *file) {
    printk(KERN_INFO "HX711: Device opened\n");
    return 0;
}

/* Hàm được gọi khi User-Space dùng lệnh close() */
static int hx711_release(struct inode *inode, struct file *file) {
    printk(KERN_INFO "HX711: Device closed\n");
    return 0;
}

/* Hàm được gọi khi User-Space dùng lệnh read() */
static ssize_t hx711_read(struct file *file, char __user *user_buffer, size_t count, loff_t *pos) {
    int32_t weight_raw = 0;
    int i, timeout = 10; // Timeout cứng 10ms
    unsigned long flags;

    // 1. Chờ DOUT xuống mức thấp (Sẵn sàng)
    while (readl(gpio_base_virtual + GPIO_DATAIN) & (1 << DOUT_PIN)) {
        // Nhường CPU cho Kernel thay vì busy-wait
        usleep_range(1000, 1000); 
        timeout--;
        if (timeout == 0) {
            printk(KERN_ERR "HX711: Timeout! Khong thay DOUT phan hoi.\n");
            return -ETIMEDOUT;
        }
    }

    // 2. Tắt ngắt cục bộ để bit-banging không bị sai lệch timing
    local_irq_save(flags);

    // 3. Đọc 24-bit dữ liệu qua kỹ thuật Bit-banging
    for (i = 0; i < 24; i++) {
        // Tạo xung HIGH trên PD_SCK
        writel((1 << SCK_PIN), gpio_base_virtual + GPIO_SETDATAOUT);
        udelay(1);
        
        weight_raw = weight_raw << 1;
        
        // Tạo xung LOW trên PD_SCK
        writel((1 << SCK_PIN), gpio_base_virtual + GPIO_CLEARDATAOUT);
        udelay(1);

        // Đọc giá trị từ DOUT
        if (readl(gpio_base_virtual + GPIO_DATAIN) & (1 << DOUT_PIN)) {
            weight_raw++;
        }
    }

    // 4. Xung thứ 25 để thiết lập Gain = 128 cho lần đọc tiếp theo
    writel((1 << SCK_PIN), gpio_base_virtual + GPIO_SETDATAOUT);
    udelay(1);
    writel((1 << SCK_PIN), gpio_base_virtual + GPIO_CLEARDATAOUT);
    udelay(1);

    local_irq_restore(flags); // Mở lại ngắt

    // 5. Xử lý bù trừ bit dấu (2's complement cho 24-bit)
    weight_raw = weight_raw ^ 0x800000;

    // 6. Copy dữ liệu từ Kernel lên không gian User
    if (copy_to_user(user_buffer, &weight_raw, sizeof(weight_raw)) != 0) {
        return -EFAULT;
    }

    return sizeof(weight_raw);
}

/* Cấu trúc khai báo hành vi của file device */
static struct file_operations fops = {
    .open = hx711_open,
    .read = hx711_read,
    .release = hx711_release,
};

/* Hàm khởi tạo Driver (Chạy khi dùng lệnh insmod) */
static int __init hx711_init(void) {
    uint32_t reg_oe;

    printk(KERN_INFO "HX711: Initializing Full Register GPIO Driver...\n");

    // 1. Đăng ký Major Number
    major_number = register_chrdev(0, DRIVER_NAME, &fops);
    if (major_number < 0) {
        printk(KERN_ALERT "HX711: Failed to register major number\n");
        return major_number;
    }

    // 2. Ánh xạ bộ nhớ cho hệ thống Clock Peripheral (CM_PER)
    cm_per_virtual = ioremap(CM_PER_BASE, 0x4000);
    if (!cm_per_virtual) {
        printk(KERN_ERR "HX711: Failed to map CM_PER memory\n");
        unregister_chrdev(major_number, DRIVER_NAME);
        return -EIO;
    }

    // 3. Bật xung nhịp cho GPIO1
    // Ghi 0x02 (MODULEMODE = ENABLE) vào thanh ghi CM_PER_GPIO1_CLKCTRL
    writel(0x02, cm_per_virtual + CM_PER_GPIO1_CLKCTRL);

    // 4. Ánh xạ bộ nhớ cho module GPIO1
    gpio_base_virtual = ioremap(GPIO1_BASE, 0x1000);
    if (!gpio_base_virtual) {
        printk(KERN_ERR "HX711: Failed to map GPIO1 memory\n");
        iounmap(cm_per_virtual);
        unregister_chrdev(major_number, DRIVER_NAME);
        return -EIO;
    }

    // 5. Cấu hình In/Out cho các chân trên thanh ghi GPIO_OE
    // Đọc giá trị hiện tại của thanh ghi OE
    reg_oe = readl(gpio_base_virtual + GPIO_OE);
    
    // Set bit DOUT lên 1 (Input)
    reg_oe |= (1 << DOUT_PIN);   
    // Set bit SCK xuống 0 (Output)
    reg_oe &= ~(1 << SCK_PIN);   
    
    // Ghi lại cấu hình vào thanh ghi OE
    writel(reg_oe, gpio_base_virtual + GPIO_OE);

    // Khởi tạo chân SCK ở mức LOW để sẵn sàng đọc
    writel((1 << SCK_PIN), gpio_base_virtual + GPIO_CLEARDATAOUT);

    printk(KERN_INFO "HX711: Hardware initialized successfully.\n");
    return 0;
}

/* Hàm dọn dẹp Driver (Chạy khi dùng lệnh rmmod) */
static void __exit hx711_exit(void) {
    // Kéo SCK xuống LOW cho an toàn trước khi thoát
    if (gpio_base_virtual) {
        writel((1 << SCK_PIN), gpio_base_virtual + GPIO_CLEARDATAOUT);
        iounmap(gpio_base_virtual);
    }
    
    // Tắt clock của GPIO1 (Ghi 0 vào CLKCTRL)
    if (cm_per_virtual) {
        writel(0x00, cm_per_virtual + CM_PER_GPIO1_CLKCTRL);
        iounmap(cm_per_virtual);
    }
    
    // Hủy đăng ký device
    unregister_chrdev(major_number, DRIVER_NAME);
    
    printk(KERN_INFO "HX711: Driver removed\n");
}

module_init(hx711_init);
module_exit(hx711_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nhom 9 - HDHN");
MODULE_DESCRIPTION("Complete Register-level GPIO Driver for HX711");
MODULE_VERSION("1.0");
