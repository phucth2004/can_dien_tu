#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

int main() {
    int fd;
    int adc_value = 0;

    fd = open("/dev/tcrt5000", O_RDONLY);
    if (fd < 0) {
        printf("Loi: Khong the mo /dev/tcrt5000\n");
        return -1;
    }

    printf("Da ket noi voi TCRT5000!\n");

    while(1) {
        if (read(fd, &adc_value, sizeof(adc_value)) == sizeof(adc_value)) {
            printf("Gia tri ADC: %d\n", adc_value);
        } else {
            printf("Loi doc du lieu\n");
        }
        usleep(500000); // Đọc mỗi 0.5s
    }

    close(fd);
    return 0;
}
