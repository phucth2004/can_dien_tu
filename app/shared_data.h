#ifndef SHARED_DATA_H
#define SHARED_DATA_H

#include <stdint.h>

#define SHM_NAME "/my_scale_shm"

typedef struct {
    int32_t weight_raw;    // Dữ liệu thô từ Kernel Driver HX711
    int ir_triggered;      // Cờ báo hiệu có người chạm/đưa vật vào
} scale_data_t;

#endif
