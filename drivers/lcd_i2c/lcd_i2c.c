#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/mutex.h>

#define DRIVER_NAME "my_bbb_lcd"
#define CLASS_NAME  "bbb_lcd_class"

/* Bitmask cho PCF8574 */
#define LCD_BL   0x08   /* Backlight bit (Mặc định ON) */
#define LCD_EN   0x04   /* Enable bit    */
#define LCD_RW   0x02   /* Read/Write bit (luôn = 0) */
#define LCD_RS   0x01   /* Register Select: 0=cmd, 1=data */

static dev_t dev_num;
static struct class  *dev_class;
static struct cdev    my_cdev;
static struct i2c_client *lcd_client;

static DEFINE_MUTEX(lcd_mutex);

/* Biến lưu trạng thái đèn nền hiện tại */
static u8 current_bl_state = LCD_BL; 

/* Hàm cập nhật ngay lập tức trạng thái đèn nền ra I2C mà không sinh xung EN */
static void lcd_update_backlight(void)
{
    u8 data = current_bl_state;
    i2c_master_send(lcd_client, &data, 1);
}

/* --- FIX GÓI XUNG EN ĐỂ TRÁNH TIMEOUT I2C --- */
static void lcd_send_4bit(u8 byte_with_flags)
{
    u8 nibble = byte_with_flags & 0xF0;          
    u8 rs     = byte_with_flags & LCD_RS;        
    /* SỬ DỤNG BIẾN current_bl_state THAY VÌ LCD_BL CỨNG */
    u8 base   = nibble | current_bl_state | rs;            
    u8 buf[3];

    buf[0] = base;              /* EN=0, đặt data lên bus */
    buf[1] = base | LCD_EN;     /* EN=1, chốt cạnh dương */
    buf[2] = base;              /* EN=0, chốt cạnh âm */

    /* Gửi 3 trạng thái trong 1 gói duy nhất */
    i2c_master_send(lcd_client, buf, 3);
    usleep_range(500, 1000);
}

static void lcd_send_byte(u8 val, u8 mode)
{
    u8 high = (val & 0xF0) | (mode & LCD_RS);
    u8 low  = ((val << 4) & 0xF0) | (mode & LCD_RS);

    lcd_send_4bit(high);
    lcd_send_4bit(low);
}

static int my_open(struct inode *inode, struct file *file) { return 0; }
static int my_release(struct inode *inode, struct file *file) { return 0; }

static ssize_t my_write(struct file *file, const char __user *user_buf,
                        size_t size, loff_t *offset)
{
    char kernel_buf[33]; 
    int  i;
    int  n = (size > 32) ? 32 : (int)size;

    if (copy_from_user(kernel_buf, user_buf, n))
        return -EFAULT;

    kernel_buf[n] = '\0';

    /* ========================================================= */
    /* BẮT LỆNH ĐIỀU KHIỂN ĐÈN NỀN TỪ USER-SPACE                 */
    /* ========================================================= */
    if (strncmp(kernel_buf, "[BL_OFF]", 8) == 0) {
        mutex_lock(&lcd_mutex);
        current_bl_state = 0x00; // Xóa bit đèn nền
        lcd_update_backlight();
        mutex_unlock(&lcd_mutex);
        return n;
    }
    if (strncmp(kernel_buf, "[BL_ON]", 7) == 0) {
        mutex_lock(&lcd_mutex);
        current_bl_state = LCD_BL; // Set bit đèn nền
        lcd_update_backlight();
        mutex_unlock(&lcd_mutex);
        return n;
    }
    /* ========================================================= */

    mutex_lock(&lcd_mutex);

    /* Đưa con trỏ về đầu (0x80) để ghi đè, CHỐNG CHỚP NHÁY màn hình cân */
    lcd_send_byte(0x80, 0x00);
    usleep_range(2000, 3000); 

    for (i = 0; i < n; i++) {
        if (kernel_buf[i] == '\0') break;
        if (kernel_buf[i] == '\n') {
            lcd_send_byte(0xC0, 0x00); /* Nhảy xuống dòng 2 */
            continue;
        }
        lcd_send_byte(kernel_buf[i], LCD_RS); 
    }

    mutex_unlock(&lcd_mutex);

    *offset = 0;
    return n;
}

static const struct file_operations fops = {
    .owner   = THIS_MODULE,
    .open    = my_open,
    .release = my_release,
    .write   = my_write,
};

static void lcd_init_hardware(void)
{
    msleep(50);
    lcd_send_4bit(0x30); msleep(5);
    lcd_send_4bit(0x30); msleep(2);
    lcd_send_4bit(0x30); msleep(2);
    lcd_send_4bit(0x20); msleep(2);

    lcd_send_byte(0x28, 0x00); usleep_range(50, 100);
    lcd_send_byte(0x08, 0x00); usleep_range(50, 100);
    lcd_send_byte(0x01, 0x00); msleep(3);
    lcd_send_byte(0x06, 0x00); usleep_range(50, 100);
    lcd_send_byte(0x0C, 0x00); usleep_range(50, 100);
    
    // Bật đèn nền mặc định khi khởi tạo xong
    current_bl_state = LCD_BL;
    lcd_update_backlight();
}

static int lcd_probe(struct i2c_client *client)
{
    int ret;
    lcd_client = client;

    lcd_init_hardware();

    cdev_init(&my_cdev, &fops);
    ret = cdev_add(&my_cdev, dev_num, 1);
    if (ret < 0) return ret;

    if (IS_ERR(device_create(dev_class, NULL, dev_num, NULL, DRIVER_NAME))) {
        cdev_del(&my_cdev);
        return -ENODEV;
    }

    pr_info("LCD Driver: Da khoi tao, /dev/%s san sang\n", DRIVER_NAME);
    return 0;
}

static void lcd_remove(struct i2c_client *client)
{
    mutex_lock(&lcd_mutex);
    lcd_send_byte(0x01, 0x00);
    msleep(2);
    
    /* Tắt hẳn đèn và chữ khi remove module */
    current_bl_state = 0x00;
    lcd_send_byte(0x08, 0x00); 
    mutex_unlock(&lcd_mutex);

    device_destroy(dev_class, dev_num);
    cdev_del(&my_cdev);
}

static const struct of_device_id lcd_dt_ids[] = {
    { .compatible = "custom,i2c-lcd", }, { }
};
MODULE_DEVICE_TABLE(of, lcd_dt_ids);

static const struct i2c_device_id lcd_ids[] = {
    { "i2c_lcd", 0 }, { }
};
MODULE_DEVICE_TABLE(i2c, lcd_ids);

static struct i2c_driver lcd_driver = {
    .driver = {
        .name           = DRIVER_NAME,
        .of_match_table = lcd_dt_ids,
    },
    .probe    = lcd_probe,
    .remove   = lcd_remove,
    .id_table = lcd_ids,
};

static int __init my_driver_init(void)
{
    int ret = alloc_chrdev_region(&dev_num, 0, 1, DRIVER_NAME);
    if (ret < 0) return ret;

    dev_class = class_create(CLASS_NAME);
    if (IS_ERR(dev_class)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(dev_class);
    }

    ret = i2c_add_driver(&lcd_driver);
    if (ret < 0) {
        class_destroy(dev_class);
        unregister_chrdev_region(dev_num, 1);
        return ret;
    }
    return 0;
}

static void __exit my_driver_exit(void)
{
    i2c_del_driver(&lcd_driver);
    class_destroy(dev_class);
    unregister_chrdev_region(dev_num, 1);
}

module_init(my_driver_init);
module_exit(my_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ban");
MODULE_DESCRIPTION("BBB I2C LCD Driver - BL Control");
