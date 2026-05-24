PHẦN 1: TẠO VÀ BIÊN DỊCH TRÊN MÁY TÍNH (HOST - UBUNTU)

Toàn bộ các bước này thực hiện trên terminal của máy tính Ubuntu, nơi chứa source code.

1. Biên dịch Kernel Driver (.ko)
Sử dụng Makefile để biên dịch source code C tầng Kernel thành các module có thể nạp vào nhân Linux.
Bash

# Di chuyển vào thư mục chứa code driver
cd ~/hdh_project/drivers

# Lệnh biên dịch chéo (Cross-compile) cho kiến trúc ARM của BeagleBone
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -C /đường/dẫn/đến/linux-kernel M=$(pwd) modules

2. Biên dịch Ứng dụng tầng User (alarm_core, sensor_node)
Sử dụng trình biên dịch GCC cho kiến trúc ARM để dịch code C thành file thực thi.
Bash

cd ~/hdh_project/app

# Dịch lõi trung tâm (Kèm thư viện pthread và rt cho Shared Memory)
arm-linux-gnueabihf-gcc alarm_core.c -o alarm_core -lrt -lpthread

# Dịch node cảm biến
arm-linux-gnueabihf-gcc sensor_node.c -o sensor_node -lrt -lpthread

3. Khởi động Server nội bộ để truyền file
Mở một cổng mạng để BeagleBone có thể truy cập và tải file về mạch. Lệnh này giữ nguyên không tắt trong suốt quá trình triển khai.
Bash

python3 -m http.server 8000

PHẦN 2: KẾT NỐI VÀ CẤU HÌNH MẠCH (TARGET - BEAGLEBONE BLACK)

Các bước này thực hiện để truy cập vào hệ điều hành của mạch (Buildroot Linux).

1. Kết nối vào mạch qua giao tiếp Serial / UART
Bash

# Mở terminal từ máy tính chui vào mạch qua cáp USB/Serial
sudo picocom -b 115200 /dev/ttyUSB0
# Hoặc dùng screen:
sudo screen /dev/ttyUSB0 115200

2. Cấu hình mạng LAN (IP Tĩnh)
Giúp mạch giao tiếp được với Server Python đang mở trên máy tính.
Bash

# Cấp IP tĩnh cho cổng mạng (Nếu cắm dây LAN)
ifconfig eth0 192.168.4.3 netmask 255.255.255.0 up

# Hoặc nếu chia sẻ mạng qua cổng cắm cáp USB
ifconfig usb0 192.168.4.3 netmask 255.255.255.0 up

# Kiểm tra kết nối về máy tính
ping 192.168.4.2

PHẦN 3: NẠP VÀ CẬP NHẬT HỆ THỐNG TRÊN MẠCH

Quy trình chuẩn mỗi khi code thay đổi, nhằm tránh lỗi xung đột hệ thống.

1. Dọn dẹp môi trường cũ (Clean up)
Bash

# Dừng kịch bản tự động và các tiến trình ứng dụng
/etc/init.d/S99scale stop 2>/dev/null
killall alarm_core sensor_node python3 2>/dev/null

# Gỡ bỏ các Driver cũ đang chạy trong Kernel
rmmod hx711_driver tcrt5000_driver buzzer_driver lcd_i2c 2>/dev/null

# Xóa các file thực thi và file thiết bị ảo cũ
rm -rf /root/alarm_core /root/sensor_node /root/cloud_sync.py /root/S99scale /root/*.ko
rm -f /dev/hx711 /dev/my_buzzer /dev/my_bbb_lcd /dev/tcrt5000

2. Tải toàn bộ hệ thống mới về mạch
Bash

cd /root

# Tải Ứng dụng và Script IoT
wget http://192.168.4.2:8000/alarm_core
wget http://192.168.4.2:8000/sensor_node
wget http://192.168.4.2:8000/cloud_sync.py
wget http://192.168.4.2:8000/S99scale

# Tải Driver Kernel
wget http://192.168.4.2:8000/lcd_i2c.ko
wget http://192.168.4.2:8000/buzzer_driver.ko
wget http://192.168.4.2:8000/hx711_driver.ko
wget http://192.168.4.2:8000/tcrt5000_driver.ko

3. Phân quyền và Cài đặt tự khởi động (Auto-Start)
Bash

# Cho phép hệ điều hành quyền chạy các file này
chmod +x alarm_core sensor_node cloud_sync.py S99scale

# Đưa script cấu hình vào đúng thư mục quản lý dịch vụ lúc khởi động
mv S99scale /etc/init.d/S99scale
chmod +x /etc/init.d/S99scale

PHẦN 4: KHỞI CHẠY HỆ THỐNG (AUTOSTART VS MANUAL RUN)
Cách 1: Khởi chạy tự động qua AutoStart (Khuyên dùng cho sản phẩm)

Phương án tối ưu nhất, kịch bản S99scale sẽ tự động đảm nhận việc nạp Driver, dò Major Number, tạo Device Node trong /dev, phân quyền và kích hoạt toàn bộ các tiến trình ngầm bao gồm cả Cloud Sync và Relay.
Bash

# Lệnh khởi chạy toàn bộ hệ thống bằng một nút bấm thông qua kịch bản tự động
/etc/init.d/S99scale start

# Lệnh dừng khẩn cấp toàn bộ hệ thống (App, Driver, Còi, LCD) khi cần bảo trì
/etc/init.d/S99scale stop

# Lệnh làm mới toàn bộ hệ thống (Dọn rác, tắt app cũ và nạp lại từ đầu) khi vừa cập nhật file
/etc/init.d/S99scale restart

CÁCH 2: GIAO TIẾP KERNEL VÀ KHỞI CHẠY BẰNG TAY (MANUAL RUN)

(Nếu không dùng kịch bản Auto-Start, đây là cách chạy thủ công để gỡ lỗi).

1. Nạp Driver và Đăng ký thiết bị I2C
Bash

insmod lcd_i2c.ko
insmod buzzer_driver.ko
insmod hx711_driver.ko
insmod tcrt5000_driver.ko

# Đánh thức chuẩn giao tiếp I2C cho màn hình LCD
echo i2c_lcd 0x27 > /sys/bus/i2c/devices/i2c-2/new_device

2. Tạo File Giao Tiếp (Device Node) với cấp quyền phần cứng
Sử dụng awk để tự động dò mã Major Number thay vì gõ tay từng số.
Bash

MAJOR_HX=$(awk '$2=="hx711" {print $1}' /proc/devices)
mknod /dev/hx711 c $MAJOR_HX 0

# Tương tự cho tcrt5000, my_buzzer, my_bbb_lcd...

# Cấp quyền đọc/ghi cho ứng dụng ở không gian người dùng (User-Space)
chmod 666 /dev/hx711 /dev/tcrt5000 /dev/my_buzzer /dev/my_bbb_lcd

3. Khởi chạy Ứng dụng
Bash

# Chạy Lõi trung tâm ở chế độ ngầm và in ra số PID
./alarm_core &

# Chạy Mắt thần IR, truyền số PID của alarm_core vừa nhận được vào lệnh
./sensor_node <SỐ_PID> &

# Khởi chạy ứng dụng đồng bộ đám mây (Google Sheets)
python3 cloud_sync.py &

PHẦN 5: KIỂM TRA VÀ GỠ LỖI (DEBUGGING & LOGGING)

Các lệnh không thể thiếu khi vận hành thực tế.
Bash

# 1. Quản lý trạng thái khởi động tự động
/etc/init.d/S99scale start   # Bật toàn bộ hệ thống
/etc/init.d/S99scale stop    # Tắt toàn bộ
/etc/init.d/S99scale restart # Khởi động lại làm mới

# 2. Kiểm tra tiến trình ứng dụng đang chạy
ps | grep -E 'alarm_core|sensor_node|python3'

# 3. Kiểm tra Driver đã được nạp chưa
lsmod

# 4. Kiểm tra mã Hardware được cấp phát
cat /proc/devices | grep -e hx711 -e tcrt -e buzzer -e lcd

# 5. Theo dõi dữ liệu ghi vào tệp log theo thời gian thực
tail -f /root/can_data.csv

PHẦN 6: KIỂM THỬ TÍNH NĂNG TỰ PHỤC HỒI (HARDWARE WATCHDOG)

Phần này hướng dẫn các kịch bản kiểm thử giả lập sự cố treo hệ thống để chứng minh khả năng tự phục hồi (Self-recovery) của mạch bằng Hardware Watchdog (15 giây) mà không cần con người can thiệp vật lý.
Kịch bản 1: Giả lập ứng dụng chính bị Treo/Chết (An toàn)

Khi ứng dụng điều khiển chính bị tắt, cơ chế nuôi chó (Feed Watchdog) bị ngắt hoàn toàn. Bộ đếm ngược phần cứng sẽ kích hoạt sau 15 giây.
Bash

# Bước 1: Giết tiến trình chính đang đảm nhận nuôi Watchdog
killall alarm_core

# Bước 2: Quan sát dòng thời gian
# - Từ giây 1 đến 29: Hệ thống im lặng, tệp can_data.csv dừng cập nhật.
# - Đúng giây 30: Watchdog phần cứng CPU đếm về 0, ép toàn bộ mạch reboot lập tức.
# - Sau khi reboot: Nhờ có AutoStart (S99scale), hệ thống sẽ tự nạp lại và chạy bình thường.

Kịch bản 2: Giả lập Kernel bị Đơ/Panic (Kiểm thử mức sâu)

Trường hợp toàn bộ hệ điều hành bị đóng băng hệ thống ngắt, không một câu lệnh nào có thể thực thi.
Bash

# Lệnh ép nhân Linux rơi vào trạng thái Kernel Panic ngay lập tức
echo c > /proc/sysrq-trigger

# Kết quả: Toàn bộ terminal bị đóng băng, đúng 30 giây sau mạch tự động reset cứng phần cứng.

Các lệnh kiểm tra thông số bộ định thời Watchdog
Bash

# Kiểm tra xem file thiết bị quản lý Watchdog phần cứng có tồn tại trong hệ thống không
ls -l /dev/watchdog

# Đọc cấu hình thời gian Timeout (Lò xo đếm ngược) hiện tại xem có đúng 30 giây không
cat /sys/class/watchdog/watchdog0/timeout
