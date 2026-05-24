/*
 * sensor_node.c - Node đọc cảm biến và gửi dữ liệu vào Shared Memory
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

#define ADC_THRESHOLD    1000  /* FIX: Che mat thi ADC ve 100, chon nguong 1000 de kich hoat */
#define DEBOUNCE_COUNT   3
#define SAMPLE_INTERVAL_US  100000  /* 100ms */
#define SLEEP_TIMEOUT_CYCLES 100   /* 10 giay */

#define TCRT5000_DEV    "/dev/tcrt5000"
#define HX711_DEV       "/dev/hx711"

int main(int argc, char *argv[])
{
    int          shm_fd, hx711_fd, adc_fd;
    scale_data_t *shared_mem;
    pid_t        target_pid;
    int32_t      raw_weight;
    
    int          last_ir_state    = 0;
    int          debounce_counter = 0;
    int          idle_counter     = 0;
    int          adc_val          = 0;

    if (argc != 2) {
        fprintf(stderr, "Cach dung: %s <PID_cua_alarm_core>\n", argv[0]);
        return -1;
    }

    target_pid = (pid_t)strtol(argv[1], NULL, 10);
    if (kill(target_pid, 0) != 0) {
        fprintf(stderr, "[SENSOR NODE] Khong tim thay process PID=%d\n", (int)target_pid);
        return -1;
    }

    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    ftruncate(shm_fd, sizeof(scale_data_t));
    shared_mem = mmap(NULL, sizeof(scale_data_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

    hx711_fd = open(HX711_DEV, O_RDONLY);
    adc_fd = open(TCRT5000_DEV, O_RDONLY);

    printf("[SENSOR NODE] Dang hoat dong (Ngui logic chuon: ADC < 1000)...\n");

    while (1) {
        /* 1. Đọc HX711 */
        if (read(hx711_fd, &raw_weight, sizeof(raw_weight)) == sizeof(raw_weight)) {
            shared_mem->weight_raw = raw_weight;
        }

        /* 2. Đọc ADC */
        if (adc_fd >= 0) {
            if (read(adc_fd, &adc_val, sizeof(adc_val)) != sizeof(adc_val)) {
                adc_val = 4095; // Mac dinh muc cao khi loi
            }
        }

        /* 3. Xử lý logic Active-Low */
        // Có vật -> ADC tụt xuống thấp (< 1000)
        int current_ir_state = (adc_val < ADC_THRESHOLD) ? 1 : 0;

        if (current_ir_state == 1) {
            debounce_counter++;
            idle_counter = 0; // Co vat -> Reset bo dem ngu dong

            if (debounce_counter == DEBOUNCE_COUNT) {
                if (last_ir_state == 0) {
                    shared_mem->ir_triggered = 1; // Lệnh WAKE UP
                    kill(target_pid, SIGUSR1);
                    printf("[SENSOR NODE] Phat hien vat (ADC=%d < 1000), gui WAKE UP\n", adc_val);
                }
                last_ir_state = 1;
            }
            if (debounce_counter > DEBOUNCE_COUNT) debounce_counter = DEBOUNCE_COUNT;

        } else {
            debounce_counter = 0;
            last_ir_state    = 0;

            idle_counter++;
            if (idle_counter == SLEEP_TIMEOUT_CYCLES) {
                shared_mem->ir_triggered = 2; // Lệnh SLEEP
                kill(target_pid, SIGUSR1);
                printf("[SENSOR NODE] 10s ranh roi (ADC=%d), gui lenh SLEEP LCD.\n", adc_val);
            }
            if (idle_counter > SLEEP_TIMEOUT_CYCLES) idle_counter = SLEEP_TIMEOUT_CYCLES + 1;
        }

        usleep(SAMPLE_INTERVAL_US);
    }

    close(adc_fd); close(hx711_fd);
    munmap(shared_mem, sizeof(scale_data_t)); close(shm_fd);
    return 0;
}
