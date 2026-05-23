/*
 * sensor_node.c - Node đọc cảm biến và gửi dữ liệu vào Shared Memory
 *
 * Đọc liên tục:
 *   - HX711 loadcell qua /dev/hx711
 *   - TCRT5000 qua /dev/tcrt5000 (kernel driver trả về int 4 byte)
 * Ghi vào Shared Memory và gửi SIGUSR1 cho alarm_core khi phát hiện vật.
 *
 * Thay đổi so với phiên bản trước:
 *   [BUG #15 FIX] Đọc ADC từ /dev/tcrt5000 (int 4 byte) thay vì sysfs string
 *                 để khớp với driver tcrt5000.c
 *   Giữ nguyên tất cả các fix khác: debounce, validate PID, error check SHM,
 *   mở file 1 lần, lseek SEEK_SET để đọc lại.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "shared_data.h"

/* ------------------------------------------------------------------ */
/*  Hằng số cấu hình                                                   */
/* ------------------------------------------------------------------ */

/*
 * Ngưỡng ADC 12-bit (0..4095) để phát hiện vật cản TCRT5000.
 * TCRT5000 phản xạ IR: khi có vật gần → điện áp cao → ADC lớn.
 * Ngưỡng mặc định 2000 ≈ ~1.6V trên dải 0-3.3V.
 * Điều chỉnh giá trị này theo khoảng cách và độ nhạy thực tế.
 */
#define ADC_THRESHOLD    2000

/*
 * Số chu kỳ liên tiếp cần đọc tín hiệu ổn định trước khi trigger.
 * SAMPLE_INTERVAL_US = 100ms → DEBOUNCE_COUNT = 3 → 300ms ổn định
 * → loại bỏ nhiễu nhất thời từ cảm biến.
 */
#define DEBOUNCE_COUNT   3

/* Chu kỳ lấy mẫu */
#define SAMPLE_INTERVAL_US  100000  /* 100ms = 10 lần/giây */

/* Đường dẫn device */
#define TCRT5000_DEV    "/dev/tcrt5000"
#define HX711_DEV       "/dev/hx711"

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    int          shm_fd, hx711_fd, adc_fd;
    scale_data_t *shared_mem;
    pid_t        target_pid;
    int32_t      raw_weight;
    int          last_ir_state    = 0;
    int          debounce_counter = 0;
    int          adc_val          = 0;

    /* ----------------------------------------------------------------
     * Validate tham số PID đầu vào
     * ---------------------------------------------------------------- */
    if (argc != 2) {
        fprintf(stderr, "Cach dung: %s <PID_cua_alarm_core>\n", argv[0]);
        return -1;
    }

    char *endptr;
    long  pid_long = strtol(argv[1], &endptr, 10);
    if (*endptr != '\0' || pid_long <= 0 || pid_long > 4194304) {
        fprintf(stderr, "[SENSOR NODE] PID khong hop le: '%s'\n", argv[1]);
        fprintf(stderr, "              PID phai la so nguyen duong (1..4194304)\n");
        return -1;
    }
    target_pid = (pid_t)pid_long;

    if (kill(target_pid, 0) != 0) {
        fprintf(stderr, "[SENSOR NODE] Khong tim thay process PID=%d: %s\n",
                (int)target_pid, strerror(errno));
        fprintf(stderr, "              Hay chay alarm_core truoc va lay dung PID.\n");
        return -1;
    }

    printf("[SENSOR NODE] Target PID = %d (da xac nhan ton tai)\n", (int)target_pid);

    /* ----------------------------------------------------------------
     * Khởi tạo Shared Memory với kiểm tra lỗi đầy đủ
     * ---------------------------------------------------------------- */
    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("[SENSOR NODE] Loi shm_open");
        return -1;
    }

    if (ftruncate(shm_fd, sizeof(scale_data_t)) == -1) {
        perror("[SENSOR NODE] Loi ftruncate");
        close(shm_fd);
        return -1;
    }

    shared_mem = mmap(NULL, sizeof(scale_data_t),
                      PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shared_mem == MAP_FAILED) {
        perror("[SENSOR NODE] Loi mmap");
        close(shm_fd);
        return -1;
    }

    memset(shared_mem, 0, sizeof(scale_data_t));

    /* ----------------------------------------------------------------
     * Mở Driver HX711
     * ---------------------------------------------------------------- */
    hx711_fd = open(HX711_DEV, O_RDONLY);
    if (hx711_fd < 0) {
        perror("[SENSOR NODE] Loi mo /dev/hx711. Kiem tra insmod va mknod!");
        munmap(shared_mem, sizeof(scale_data_t));
        close(shm_fd);
        return -1;
    }

    /* ----------------------------------------------------------------
     * [BUG #15 FIX] Mở driver TCRT5000 (/dev/tcrt5000) thay vì sysfs.
     *
     * Driver tcrt5000.c trả về int (4 byte, giá trị 0..4095).
     * Mỗi lần read() trigger 1 lần chuyển đổi ADC (one-shot mode).
     *
     * Khác với sysfs:
     *   sysfs  → open/lseek/read chuỗi ASCII "1234\n" → atoi()
     *   driver → read() trực tiếp trả về int nhị phân 4 byte
     * ---------------------------------------------------------------- */
    adc_fd = open(TCRT5000_DEV, O_RDONLY);
    if (adc_fd < 0) {
        fprintf(stderr, "[SENSOR NODE] Canh bao: Khong mo duoc %s: %s\n",
                TCRT5000_DEV, strerror(errno));
        fprintf(stderr, "              Kiem tra: insmod tcrt5000.ko\n");
        fprintf(stderr, "              He thong se chay khong co cam bien vat can.\n");
        /* Không thoát — vẫn có thể chạy chỉ với HX711 */
    }

    printf("[SENSOR NODE] Dang hoat dong.\n");
    printf("[SENSOR NODE]   ADC device : %s\n", TCRT5000_DEV);
    printf("[SENSOR NODE]   ADC nguong : %d / 4095 (12-bit)\n", ADC_THRESHOLD);
    printf("[SENSOR NODE]   Debounce   : %d chu ky x %dms = %dms\n",
           DEBOUNCE_COUNT, SAMPLE_INTERVAL_US / 1000,
           DEBOUNCE_COUNT * SAMPLE_INTERVAL_US / 1000);

    /* ================================================================
     * Vòng lặp chính
     * ================================================================ */
    while (1) {

        /* ---- 1. Đọc HX711 → Shared Memory ---- */
        if (read(hx711_fd, &raw_weight, sizeof(raw_weight)) == sizeof(raw_weight)) {
            shared_mem->weight_raw = raw_weight;
        }

        /* ---- 2. Đọc ADC từ driver TCRT5000 ----
         *
         * [BUG #15 FIX] Mỗi lần read() gọi tcrt_read() trong driver,
         * driver tự trigger ADC one-shot và trả về 4 byte int.
         * Không cần lseek() vì driver tự reset *pos = 0 sau mỗi read.
         */
        if (adc_fd >= 0) {
            if (read(adc_fd, &adc_val, sizeof(adc_val)) != sizeof(adc_val)) {
                /* Lỗi đọc ADC — log nhưng không thoát */
                if (errno != EAGAIN)
                    fprintf(stderr, "[SENSOR NODE] Canh bao: loi doc ADC: %s\n",
                            strerror(errno));
                adc_val = 0;
            }
        }

        /* ---- 3. Debounce + Gửi tín hiệu ----
         *
         * Tăng bộ đếm khi tín hiệu = 1, reset khi = 0.
         * Chỉ trigger khi đếm đúng bằng DEBOUNCE_COUNT (cạnh dương
         * của debounce) để loại bỏ nhiễu nhất thời.
         */
        int current_ir_state = (adc_val < ADC_THRESHOLD) ? 1 : 0;

        if (current_ir_state == 1) {
            debounce_counter++;

            if (debounce_counter == DEBOUNCE_COUNT) {
                if (last_ir_state == 0) {
                    shared_mem->ir_triggered = 1;

                    if (kill(target_pid, SIGUSR1) != 0) {
                        fprintf(stderr,
                                "[SENSOR NODE] Loi gui SIGUSR1 den PID=%d: %s\n",
                                (int)target_pid, strerror(errno));
                    } else {
                        printf("[SENSOR NODE] Phat hien vat (ADC=%d / 4095), da gui SIGUSR1\n",
                               adc_val);
                    }
                }
                last_ir_state = 1;
            }

            /* Giới hạn để tránh overflow */
            if (debounce_counter > DEBOUNCE_COUNT)
                debounce_counter = DEBOUNCE_COUNT;

        } else {
            debounce_counter = 0;
            last_ir_state    = 0;
        }

        usleep(SAMPLE_INTERVAL_US);
    }

    /* Cleanup */
    if (adc_fd >= 0) close(adc_fd);
    close(hx711_fd);
    munmap(shared_mem, sizeof(scale_data_t));
    close(shm_fd);

    return 0;
}
