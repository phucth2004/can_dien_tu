#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

int main() {
    int fd;
    int32_t weight_raw;

    // Mở file giao tiếp do driver tạo ra
    fd = open("/dev/hx711", O_RDONLY);
    if (fd < 0) {
        printf("Loi: Khong the mo /dev/hx711. Kiem tra lai mknod.\n");
        return -1;
    }

    printf("Ket noi thanh cong voi module HX711!\n");
    printf("Dang doc du lieu Loadcell (24-bit)...\n");

    while(1) {
        if (read(fd, &weight_raw, sizeof(weight_raw)) == sizeof(weight_raw)) {
            printf("Gia tri Loadcell Raw: %d\n", weight_raw);
        } else {
            printf("Loi doc du lieu hoac Timeout!\n");
        }
        usleep(500000); // Đọc mỗi 0.5 giây
    }

    close(fd);
    return 0;
}
