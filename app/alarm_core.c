/*
 * alarm_core.c - Lõi xử lý chính của hệ thống cân điện tử tự động
 *
 * Nhận tín hiệu SIGUSR1 từ sensor_node khi phát hiện vật,
 * đọc trọng lượng từ Shared Memory, hiển thị kết quả lên LCD
 * và điều khiển còi PWM theo tiêu chuẩn PASS/FAIL.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include "shared_data.h"

#define SHM_NAME "/my_scale_shm"

/*
 * [NEW FIX] Thay vì Hardcode, chúng ta dùng biến toàn cục để lưu Mốc 0.
 * Hệ số SCALE tạm giữ 420 theo code của bạn (có thể đổi thành 465 nếu cân bị nhẹ hơn thực tế)
 */
long weight_zero_offset = 0; 
#define WEIGHT_SCALE        420.0       

/* Ngưỡng an toàn để nhận biết đĩa cân đã trống (~12g) */
#define WEIGHT_EMPTY_THRESHOLD  (weight_zero_offset + 5000) 

/* Ngưỡng sản phẩm đạt chuẩn (gram) */
#define PRODUCT_MIN_G  240
#define PRODUCT_MAX_G  260

/* Shared state */
scale_data_t           *shared_mem;
static volatile sig_atomic_t start_weighing = 0;
static pthread_mutex_t i2c_mutex = PTHREAD_MUTEX_INITIALIZER;


/* ------------------------------------------------------------------ */
/* Hàm Tự Động Trừ Bì (Auto-Tare)                                    */
/* ------------------------------------------------------------------ */
static void calibrate_scale(void)
{
    printf("[ALARM CORE] Dang cho sensor_node ket noi de lay moc 0...\n");
    
    /* Vì alarm_core chạy trước, phải chờ sensor_node ghi giá trị raw thật vào SHM */
    while (shared_mem->weight_raw == 0) {
        usleep(100000); 
    }

    printf("[ALARM CORE] Bat dau tru bi (Tare). Vui long khong dat vat len can...\n");
    long sum = 0;
    
    /* Chờ 0.5s cho cân thật sự ổn định sau khi bật mắt thần */
    usleep(500000); 

    /* Quét 10 mẫu để lấy trung bình cộng */
    for (int i = 0; i < 10; i++) {
        sum += shared_mem->weight_raw;
        usleep(100000); /* Đọc mỗi 100ms */
    }
    
    weight_zero_offset = sum / 10;
    printf("[ALARM CORE] Da tru bi xong! MOC 0 (OFFSET) = %ld\n", weight_zero_offset);
}


/* ------------------------------------------------------------------ */
/* Signal & Hardware Helpers                                         */
/* ------------------------------------------------------------------ */
static void signal_handler(int sig)
{
    if (sig == SIGUSR1)
        start_weighing = 1;
}

static void play_buzzer(int fd, const char *mode)
{
    if (fd < 0) return;

    if (strcmp(mode, "TING_TING") == 0) {
        write(fd, "1", 1); usleep(200000); write(fd, "2", 1);
    } else if (strcmp(mode, "PASS") == 0) {
        write(fd, "1", 1); usleep(150000);
        write(fd, "2", 1); usleep(100000);
        write(fd, "1", 1); usleep(150000);
        write(fd, "2", 1);
    } else if (strcmp(mode, "FAIL") == 0) {
        write(fd, "0", 1); usleep(600000);
        write(fd, "2", 1);
    } else if (strcmp(mode, "OFF") == 0) {
        write(fd, "2", 1);
    }
}

static void lcd_print_safe(int fd, const char *text)
{
    if (fd < 0) return;
    pthread_mutex_lock(&i2c_mutex);
    write(fd, text, strlen(text));
    pthread_mutex_unlock(&i2c_mutex);
}


/* ------------------------------------------------------------------ */
/* Main                                                              */
/* ------------------------------------------------------------------ */
int main(void)
{
    int shm_fd, buzzer_fd, lcd_fd;

    printf("[ALARM CORE] PID cua toi la: %d\n", getpid());
    printf("[ALARM CORE] Nguong PASS: %d - %d g\n", PRODUCT_MIN_G, PRODUCT_MAX_G);

    signal(SIGUSR1, signal_handler);

    /* 1. Thiết lập Shared Memory */
    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) { perror("[ALARM CORE] Loi tao Shared Memory"); return -1; }
    if (ftruncate(shm_fd, sizeof(scale_data_t)) == -1) { perror("[ALARM CORE] Loi ftruncate"); close(shm_fd); return -1; }
    
    shared_mem = mmap(NULL, sizeof(scale_data_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shared_mem == MAP_FAILED) { perror("[ALARM CORE] Loi mmap"); close(shm_fd); return -1; }

    /* 2. Mở Driver Còi & LCD */
    buzzer_fd = open("/dev/my_buzzer", O_WRONLY);
    if (buzzer_fd < 0) printf("[ALARM CORE] Canh bao: Khong mo duoc /dev/my_buzzer.\n");

    lcd_fd = open("/dev/my_bbb_lcd", O_WRONLY);
    if (lcd_fd < 0) printf("[ALARM CORE] Canh bao: Khong mo duoc /dev/my_bbb_lcd.\n");

    lcd_print_safe(lcd_fd, " HE THONG CAN \n  SAN SANG!  ");
    play_buzzer(buzzer_fd, "OFF");

    /* 3. GỌI HÀM LẤY MỐC 0 NGAY TRƯỚC KHI VÀO VÒNG LẶP */
    calibrate_scale();

    printf("[ALARM CORE] He thong san sang. Dang cho vat...\n");

    /* 4. Vòng lặp chính */
    while (1) {
        if (start_weighing) {
            shared_mem->ir_triggered = 0;
            printf("[ALARM CORE] Phat hien vat, bat dau can...\n");

            lcd_print_safe(lcd_fd, "  DANG CAN... \n              ");
            play_buzzer(buzzer_fd, "TING_TING");

            /* Chờ vật ổn định trên đĩa cân (1 giây) */
            usleep(1000000);

            /* [NEW FIX] Tính khối lượng với Mốc 0 đã tự động cập nhật */
            int32_t raw    = (int32_t)shared_mem->weight_raw;
            int32_t weight_g = (raw - weight_zero_offset) / WEIGHT_SCALE;
            
            /* Lọc nhiễu âm rác */
            if (weight_g < 0 && weight_g >= -5) weight_g = 0;
            if (weight_g < 0) weight_g = 0;

            printf("[ALARM CORE] Raw=%d, Offset=%ld, Weight=%d g\n", raw, weight_zero_offset, weight_g);

            char lcd_buf[40];

            /* Phân loại PASS / FAIL */
            if (weight_g >= PRODUCT_MIN_G && weight_g <= PRODUCT_MAX_G) {
                snprintf(lcd_buf, sizeof(lcd_buf), "PASS: %d g\nCHUAN KHOI LUONG", (int)weight_g);
                lcd_print_safe(lcd_fd, lcd_buf);
                play_buzzer(buzzer_fd, "PASS");
                printf("[ALARM CORE] PASS - %d g\n", (int)weight_g);
            } else {
                snprintf(lcd_buf, sizeof(lcd_buf), "FAIL: %d g\nLOI KHOI LUONG! ", (int)weight_g);
                lcd_print_safe(lcd_fd, lcd_buf);
                play_buzzer(buzzer_fd, "FAIL");
                printf("[ALARM CORE] FAIL - %d g (ngoai nguong %d-%d g)\n", (int)weight_g, PRODUCT_MIN_G, PRODUCT_MAX_G);
            }

            /* Chờ nhấc vật ra khỏi đĩa cân */
            printf("[ALARM CORE] Cho nhat vat ra...\n");
            while ((int32_t)shared_mem->weight_raw > WEIGHT_EMPTY_THRESHOLD) {
                usleep(200000);
            }

            /* Reset về màn hình chờ */
            play_buzzer(buzzer_fd, "OFF");
            lcd_print_safe(lcd_fd, " HE THONG CAN \n  SAN SANG!  ");
            printf("[ALARM CORE] San sang can vat tiep theo.\n");

            start_weighing = 0;
        }

        usleep(50000);
    }

    close(buzzer_fd); close(lcd_fd);
    munmap(shared_mem, sizeof(scale_data_t));
    close(shm_fd);
    return 0;
}
