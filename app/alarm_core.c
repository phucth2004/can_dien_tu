#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <linux/watchdog.h>
#include "shared_data.h"

#define SHM_NAME    "/my_scale_shm"
/*
 * RELAY_URL: địa chỉ máy tính đang chạy relay_server.py.
 * 192.168.4.2 = IP máy tính trong mạng WiFi hotspot BBB tạo ra.
 */
#define RELAY_URL   "http://192.168.4.2:8000"

/* ================================================================== */
#define WEIGHT_SCALE        956L

#define TARE_WARMUP_SAMPLES 30
#define TARE_SAMPLES        50
#define WEIGHT_SAMPLES      20
#define EMPTY_HYSTERESIS    5000L
#define PRODUCT_MIN_G       0
#define PRODUCT_MAX_G       800
#define STUCK_TIMEOUT_TICKS 50
/* ================================================================== */

static long             weight_zero_offset = 0;
scale_data_t           *shared_mem;
static volatile sig_atomic_t start_weighing = 0;
static pthread_mutex_t i2c_mutex = PTHREAD_MUTEX_INITIALIZER;
static int wdt_fd = -1;

static inline long weight_empty_threshold(void)
{
    return weight_zero_offset + EMPTY_HYSTERESIS;
}

/* ================================================================== */
/*  Helpers                                                            */
/* ================================================================== */

static void feed_watchdog(void)
{
    if (wdt_fd != -1) {
        int dummy = 0;
        ioctl(wdt_fd, WDIOC_KEEPALIVE, &dummy);
    }
}

static void signal_handler(int sig)
{
    if (sig == SIGUSR1) start_weighing = 1;
}

static void play_buzzer(int fd, const char *mode)
{
    if (fd < 0) return;
    if      (strcmp(mode, "TING_TING") == 0) { write(fd,"1",1); usleep(200000); write(fd,"2",1); }
    else if (strcmp(mode, "PASS")      == 0) { write(fd,"1",1); usleep(150000); write(fd,"2",1); usleep(100000); write(fd,"1",1); usleep(150000); write(fd,"2",1); }
    else if (strcmp(mode, "FAIL")      == 0) { write(fd,"0",1); usleep(600000); write(fd,"2",1); }
    else if (strcmp(mode, "OFF")       == 0) { write(fd,"2",1); }
    else if (strcmp(mode, "WARN")      == 0) { write(fd,"0",1); usleep(300000); write(fd,"2",1); usleep(200000); write(fd,"0",1); usleep(300000); write(fd,"2",1); }
}

static void lcd_print_safe(int fd, const char *text)
{
    if (fd < 0) return;
    pthread_mutex_lock(&i2c_mutex);
    write(fd, text, strlen(text));
    pthread_mutex_unlock(&i2c_mutex);
}

/* ================================================================== */
/*  Đọc trung bình N mẫu (trimmed mean)                               */
/* ================================================================== */

static long read_average(int n)
{
    long *buf = malloc(n * sizeof(long));
    if (!buf) return (long)(int32_t)shared_mem->weight_raw;

    for (int i = 0; i < n; i++) {
        buf[i] = (long)(int32_t)shared_mem->weight_raw;
        usleep(100000);
        if (i > 0 && i % 20 == 0) feed_watchdog();
    }

    for (int i = 1; i < n; i++) {
        long k = buf[i]; int j = i - 1;
        while (j >= 0 && buf[j] > k) { buf[j+1] = buf[j]; j--; }
        buf[j+1] = k;
    }

    int trim = n / 10;
    long sum = 0;
    for (int i = trim; i < n - trim; i++) sum += buf[i];
    free(buf);
    return sum / (n - 2 * trim);
}

/* ================================================================== */
/*  Tare                                                               */
/* ================================================================== */

static void calibrate_scale(int lcd_fd, int buzzer_fd)
{
    printf("[TARE] Cho sensor_node ket noi...\n");
    lcd_print_safe(lcd_fd, "  CHO KET NOI  \n  SENSOR...   ");

    int wait_ticks = 0;
    while (shared_mem->weight_raw == 0) {
        usleep(100000);
        if (++wait_ticks >= 20) { feed_watchdog(); wait_ticks = 0; }
    }

    printf("[TARE] Warmup %d giay — DE TRONG DIA CAN!\n", TARE_WARMUP_SAMPLES / 10);
    lcd_print_safe(lcd_fd, "  KHOI DONG... \n DE TRONG CAN!");
    play_buzzer(buzzer_fd, "TING_TING");

    for (int i = 0; i < TARE_WARMUP_SAMPLES; i++) {
        usleep(100000);
        if (i > 0 && i % 20 == 0) feed_watchdog();
    }

    printf("[TARE] Dang lay mau tru bi (%d mau / %ds)...\n",
           TARE_SAMPLES, TARE_SAMPLES / 10);
    lcd_print_safe(lcd_fd, "  TRU BI...    \n DE TRONG CAN!");

    weight_zero_offset = read_average(TARE_SAMPLES);

    printf("[TARE] Hoan thanh! OFFSET = %ld, NGUONG_TRONG = %ld\n",
           weight_zero_offset, weight_empty_threshold());
    printf("[CAL] WEIGHT_SCALE = (raw - %ld) / khoi_luong_vat_g\n",
           weight_zero_offset);
}

/* ================================================================== */
/*  Gửi dữ liệu lên relay_server.py + ghi CSV backup                  */
/* ================================================================== */

static void relay_send(long weight_g, const char *status)
{
    /* Tạo timestamp ISO */
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[24];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);
    /* URL-encode dấu cách thành %20 */
    char ts_enc[32]; int j = 0;
    for (int i = 0; ts[i] && j < 30; i++) {
        if (ts[i] == ' ') { ts_enc[j++]='%'; ts_enc[j++]='2'; ts_enc[j++]='0'; }
        else ts_enc[j++] = ts[i];
    }
    ts_enc[j] = '\0';

    /*
     * wget chạy background (&) — không block alarm_core
     * -q: im lặng  -O /dev/null: bỏ output  --timeout=5: không chờ lâu
     */
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "wget -q -O /dev/null --timeout=5 "
             "\"" RELAY_URL "/?timestamp=%s&weight=%ld&status=%s\" &",
             ts_enc, weight_g, status);
    system(cmd);

    printf("[RELAY] Gui: %s | %ldg | %s\n", ts, weight_g, status);
}

/* ================================================================== */
/*  Main                                                               */
/* ================================================================== */

int main(void)
{
    int shm_fd, buzzer_fd, lcd_fd;

    printf("[ALARM CORE] PID cua toi la: %d\n", getpid());
    signal(SIGUSR1, signal_handler);

    wdt_fd = open("/dev/watchdog", O_WRONLY);
    if (wdt_fd != -1) {
        int timeout = 30;
        ioctl(wdt_fd, WDIOC_SETTIMEOUT, &timeout);
        printf("[WATCHDOG] Kich hoat, timeout=30s\n");
    }

    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) { perror("[ALARM CORE] Loi shm_open"); return -1; }
    ftruncate(shm_fd, sizeof(scale_data_t));
    shared_mem = mmap(NULL, sizeof(scale_data_t),
                      PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shared_mem == MAP_FAILED) { perror("[ALARM CORE] Loi mmap"); return -1; }

    buzzer_fd = open("/dev/my_buzzer",  O_WRONLY);
    lcd_fd    = open("/dev/my_bbb_lcd", O_WRONLY);

    play_buzzer(buzzer_fd, "OFF");
    calibrate_scale(lcd_fd, buzzer_fd);

    lcd_print_safe(lcd_fd, " HE THONG CAN \n  SAN SANG!  ");
    printf("[ALARM CORE] San sang. WEIGHT_SCALE=%ld\n", WEIGHT_SCALE);

    int loop_counter = 0;

    while (1) {
        if (start_weighing) {
            int action = shared_mem->ir_triggered;
            shared_mem->ir_triggered = 0;
            start_weighing = 0;

            if (action == 2) {
                printf("[ALARM CORE] SLEEP MODE\n");
                lcd_print_safe(lcd_fd, "[BL_OFF]");
            }
            else if (action == 1) {
                printf("[ALARM CORE] WAKE UP! Bat dau can...\n");
                lcd_print_safe(lcd_fd, "[BL_ON]");
                lcd_print_safe(lcd_fd, "  DANG CAN... \n              ");
                play_buzzer(buzzer_fd, "TING_TING");

                usleep(3000000);

                long raw_avg  = read_average(WEIGHT_SAMPLES);
                long weight_g = (raw_avg - weight_zero_offset) / WEIGHT_SCALE;
                if (weight_g < 0) weight_g = 0;

                printf("[DEBUG] raw_avg=%ld, offset=%ld, scale=%ld => %ldg\n",
                       raw_avg, weight_zero_offset, WEIGHT_SCALE, weight_g);

                char        lcd_buf[40];
                const char *status_str;

                if (weight_g >= PRODUCT_MIN_G && weight_g <= PRODUCT_MAX_G) {
                    snprintf(lcd_buf, sizeof(lcd_buf),
                             "PASS: %ld g\nCHUAN KHOI LUONG", weight_g);
                    lcd_print_safe(lcd_fd, lcd_buf);
                    play_buzzer(buzzer_fd, "PASS");
                    status_str = "PASS";
                } else {
                    snprintf(lcd_buf, sizeof(lcd_buf),
                             "FAIL: %ld g\nLOI KHOI LUONG! ", weight_g);
                    lcd_print_safe(lcd_fd, lcd_buf);
                    play_buzzer(buzzer_fd, "FAIL");
                    status_str = "FAIL";
                }

                relay_send(weight_g, status_str);

                printf("[ALARM CORE] Cho nhat vat ra (nguong=%ld)...\n",
                       weight_empty_threshold());

                int wait_ticks  = 0;
                int stuck_ticks = 0;

                while ((long)(int32_t)shared_mem->weight_raw > weight_empty_threshold()) {
                    usleep(200000);
                    if (++wait_ticks >= 10) { feed_watchdog(); wait_ticks = 0; }
                    if (++stuck_ticks == STUCK_TIMEOUT_TICKS) {
                        printf("[ALARM CORE] VAT KET TREN CAN!\n");
                        lcd_print_safe(lcd_fd, "LOI: VAT KET!  \nNHAC VAT RA!  ");
                        play_buzzer(buzzer_fd, "WARN");
                    }
                }

                play_buzzer(buzzer_fd, "OFF");
                lcd_print_safe(lcd_fd, " HE THONG CAN \n  SAN SANG!  ");
                printf("[ALARM CORE] San sang cho vat tiep theo.\n");
            }
        }

        usleep(50000);
        if (++loop_counter >= 40) { feed_watchdog(); loop_counter = 0; }
    }

    if (wdt_fd != -1) { write(wdt_fd, "V", 1); close(wdt_fd); }
    close(buzzer_fd); close(lcd_fd);
    munmap(shared_mem, sizeof(scale_data_t));
    close(shm_fd);
    return 0;
}
