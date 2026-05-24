/*
 * calibrate.c - Công cụ hiệu chuẩn cân điện tử HX711
 *               Chạy 1 lần trước khi dùng hệ thống thật
 *
 * Cách dùng:
 *   1. Tắt alarm_core và sensor_node nếu đang chạy
 *   2. insmod hx711.ko (driver phải đang nạp)
 *   3. Cross-compile và copy sang BBB:
 *        arm-buildroot-linux-gnueabihf-gcc -o calibrate calibrate.c
 *        scp calibrate root@<IP_BBB>:/root/
 *   4. Trên BBB: ./calibrate
 *   5. Làm theo hướng dẫn trên màn hình
 *   6. Ghi lại WEIGHT_SCALE và ZERO_OFFSET in ra cuối cùng
 *   7. Cập nhật 2 giá trị đó vào alarm_core.c rồi build lại
 *
 * Quy trình hiệu chuẩn:
 *   Bước 1: Tare — đo giá trị khi đĩa cân TRỐNG → tính zero_offset
 *   Bước 2: Span — đặt vật chuẩn đã biết khối lượng → tính weight_scale
 *   Bước 3: Verify — đặt vật khác để kiểm tra độ chính xác
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define HX711_DEV       "/dev/hx711"
#define WARMUP_SAMPLES  30      /* 30 × 100ms = 3s chờ loadcell ổn định */
#define TARE_SAMPLES    50      /* 50 × 100ms = 5s lấy mẫu tare         */
#define MEASURE_SAMPLES 20      /* 20 × 100ms = 2s lấy mẫu đo           */

/* ------------------------------------------------------------------ */
/* Đọc một giá trị raw từ driver                                       */
/* ------------------------------------------------------------------ */
static int read_raw(int fd, int32_t *out)
{
    int32_t val;
    if (read(fd, &val, sizeof(val)) != sizeof(val)) {
        fprintf(stderr, "[CAL] Loi doc /dev/hx711: %s\n", strerror(errno));
        return -1;
    }
    *out = val;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Đọc N mẫu, lấy trimmed mean (bỏ 10% đầu và cuối)                   */
/* ------------------------------------------------------------------ */
static long read_average(int fd, int n, const char *label)
{
    long *buf = malloc(n * sizeof(long));
    if (!buf) { perror("malloc"); exit(1); }

    printf("  Dang lay %d mau", n);
    fflush(stdout);

    for (int i = 0; i < n; i++) {
        int32_t raw;
        if (read_raw(fd, &raw) < 0) { free(buf); return -1; }
        buf[i] = (long)raw;
        usleep(100000); /* 100ms — phù hợp HX711 10Hz */
        if ((i + 1) % 10 == 0) { printf("."); fflush(stdout); }
    }
    printf(" xong\n");

    /* Sắp xếp tăng dần */
    for (int i = 1; i < n; i++) {
        long k = buf[i]; int j = i - 1;
        while (j >= 0 && buf[j] > k) { buf[j+1] = buf[j]; j--; }
        buf[j+1] = k;
    }

    /* Trimmed mean: bỏ 10% đầu và 10% cuối */
    int trim = n / 10;
    long sum = 0;
    for (int i = trim; i < n - trim; i++) sum += buf[i];

    long avg = sum / (n - 2 * trim);
    printf("  [%s] Raw trung binh = %ld (min=%ld, max=%ld)\n",
           label, avg, buf[0], buf[n-1]);

    free(buf);
    return avg;
}

/* ------------------------------------------------------------------ */
/* In separator                                                        */
/* ------------------------------------------------------------------ */
static void separator(void)
{
    printf("\n============================================================\n");
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */
int main(void)
{
    int fd;
    long zero_offset   = 0;
    long weight_scale  = 0;
    char input[32];

    separator();
    printf("  CONG CU HIEU CHUAN CAN DIEN TU HX711\n");
    printf("  BeagleBone Black - Nhom 9 HDHN\n");
    separator();

    /* Mở driver */
    fd = open(HX711_DEV, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "\n[LOI] Khong mo duoc %s: %s\n", HX711_DEV, strerror(errno));
        fprintf(stderr, "  Kiem tra: insmod hx711.ko\n");
        fprintf(stderr, "  Kiem tra: alarm_core va sensor_node PHAI duoc dung truoc\n");
        return 1;
    }
    printf("\n[OK] Da mo %s thanh cong\n", HX711_DEV);

    /* ================================================================
     * BƯỚC 0: Warmup — chờ loadcell ổn định
     * ================================================================ */
    separator();
    printf("BUOC 0: WARMUP\n\n");
    printf("  Loadcell can %d giay de on dinh sau khi cap nguon.\n",
           WARMUP_SAMPLES / 10);
    printf("  DE TRONG DIA CAN trong khi cho...\n\n");

    printf("  Dang warmup");
    fflush(stdout);
    for (int i = 0; i < WARMUP_SAMPLES; i++) {
        int32_t dummy;
        read_raw(fd, &dummy);
        usleep(100000);
        if ((i + 1) % 10 == 0) { printf("."); fflush(stdout); }
    }
    printf(" xong\n");

    /* ================================================================
     * BƯỚC 1: TARE — đo khi đĩa cân trống
     * ================================================================ */
    separator();
    printf("BUOC 1: TARE (TRU BI)\n\n");
    printf("  Hay DAM BAO dia can TRONG RONG (khong co vat gi tren do).\n");
    printf("  Nhan [Enter] khi san sang...");
    fflush(stdout);
    fgets(input, sizeof(input), stdin);

    printf("\n  Dang do gia tri nen (zero offset)...\n");
    zero_offset = read_average(fd, TARE_SAMPLES, "TARE");

    if (zero_offset < 0) {
        fprintf(stderr, "[LOI] Khong lay duoc mau tare\n");
        close(fd);
        return 1;
    }

    printf("\n  => ZERO OFFSET = %ld\n", zero_offset);

    /* ================================================================
     * BƯỚC 2: SPAN — đặt vật chuẩn, tính weight_scale
     * ================================================================ */
    separator();
    printf("BUOC 2: SPAN (HIEU CHUAN)\n\n");
    printf("  Hay dat VAT CHUAN len dia can.\n");
    printf("  Vat chuan nen co khoi luong ro rang (VD: 200g, 500g).\n\n");

    float known_weight = 0;
    while (known_weight <= 0) {
        printf("  Nhap khoi luong vat chuan (gram): ");
        fflush(stdout);
        if (fgets(input, sizeof(input), stdin)) {
            known_weight = atof(input);
            if (known_weight <= 0)
                printf("  [!] Khoi luong phai > 0, nhap lai.\n");
        }
    }

    printf("\n  Vat chuan: %.1f gram\n", known_weight);
    printf("  Dat vat len dia can, doi vat ON DINH roi nhan [Enter]...");
    fflush(stdout);
    fgets(input, sizeof(input), stdin);

    printf("\n  Dang do gia tri vat chuan...\n");
    long raw_with_weight = read_average(fd, MEASURE_SAMPLES, "SPAN");

    if (raw_with_weight < 0) {
        fprintf(stderr, "[LOI] Khong lay duoc mau span\n");
        close(fd);
        return 1;
    }

    long raw_delta = raw_with_weight - zero_offset;
    if (raw_delta <= 0) {
        fprintf(stderr, "\n[LOI] raw_delta = %ld <= 0!\n", raw_delta);
        fprintf(stderr, "  Kiem tra:\n");
        fprintf(stderr, "  - Day du tare o buoc 1 chua?\n");
        fprintf(stderr, "  - Loadcell co dung chieu khong?\n");
        fprintf(stderr, "  - Ket noi day E+/E-/A+/A- co dung khong?\n");
        close(fd);
        return 1;
    }

    weight_scale = raw_delta / (long)known_weight;

    printf("\n  raw voi vat    = %ld\n", raw_with_weight);
    printf("  zero_offset    = %ld\n", zero_offset);
    printf("  raw_delta      = %ld\n", raw_delta);
    printf("  known_weight   = %.1f g\n", known_weight);
    printf("\n  => WEIGHT_SCALE = %ld / %.0f = %ld\n",
           raw_delta, known_weight, weight_scale);

    /* ================================================================
     * BƯỚC 3: VERIFY — kiểm tra với vật khác
     * ================================================================ */
    separator();
    printf("BUOC 3: KIEM TRA (VERIFY)\n\n");
    printf("  Hay dat mot VAT KHAC len dia can de kiem tra do chinh xac.\n");
    printf("  (Co the dung lai vat chuan de xac nhan, hoac vat khac)\n\n");

    char do_verify = 'y';
    while (do_verify == 'y' || do_verify == 'Y') {
        float verify_weight = 0;
        while (verify_weight <= 0) {
            printf("  Nhap khoi luong vat kiem tra (gram): ");
            fflush(stdout);
            if (fgets(input, sizeof(input), stdin))
                verify_weight = atof(input);
        }

        printf("  Dat vat len dia can, nhan [Enter]...");
        fflush(stdout);
        fgets(input, sizeof(input), stdin);

        printf("\n  Dang do...\n");
        long raw_verify = read_average(fd, MEASURE_SAMPLES, "VERIFY");
        long measured   = (raw_verify - zero_offset) / weight_scale;

        float error_g   = (float)measured - verify_weight;
        float error_pct = (error_g / verify_weight) * 100.0f;

        printf("\n  Thuc te     : %.1f g\n", verify_weight);
        printf("  Can do duoc : %ld g\n", measured);
        printf("  Sai lech    : %+.1f g (%+.2f%%)\n", error_g, error_pct);

        if (error_pct > -2.0f && error_pct < 2.0f)
            printf("  => KET QUA: TOT (sai lech < 2%%)\n");
        else if (error_pct > -5.0f && error_pct < 5.0f)
            printf("  => KET QUA: CHAP NHAN DUOC (sai lech < 5%%)\n");
        else
            printf("  => KET QUA: CAN HIEU CHUAN LAI (sai lech > 5%%)\n");

        printf("\n  Kiem tra them vat khac? (y/n): ");
        fflush(stdout);
        if (fgets(input, sizeof(input), stdin))
            do_verify = input[0];
        else
            do_verify = 'n';
    }

    /* ================================================================
     * KẾT QUẢ CUỐI — in ra 2 hằng số cần cập nhật vào alarm_core.c
     * ================================================================ */
    separator();
    printf("KET QUA HIEU CHUAN\n");
    separator();
    printf("\n  Copy 2 dong sau vao alarm_core.c (thay the gia tri cu):\n\n");
    printf("  #define WEIGHT_SCALE   %ldL\n", weight_scale);
    printf("\n  Trong ham calibrate_scale(), sau khi tinh weight_zero_offset,\n");
    printf("  gia tri offset se duoc tinh tu dong khi chay.\n");
    printf("  (khong can sua ZERO_OFFSET trong code — no duoc tinh lai moi lan boot)\n");
    printf("\n  WEIGHT_SCALE cuoi cung: %ld\n", weight_scale);
    printf("  ZERO_OFFSET mau nay  : %ld  (chi de tham khao, thay doi theo nhiet do)\n",
           zero_offset);
    separator();
    printf("\n  Sau khi cap nhat alarm_core.c:\n");
    printf("    make\n");
    printf("    scp alarm_core root@<IP_BBB>:/usr/bin/\n");
    printf("    /etc/init.d/S99scale restart\n\n");

    close(fd);
    return 0;
}
