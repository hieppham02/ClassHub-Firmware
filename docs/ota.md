# Cập nhật firmware ClassHub qua Wi-Fi

## Cài lần đầu qua USB–UART

Board dùng ESP32-S3 N16R8 (flash 16 MB). Bản hiện tại bổ sung OTA vào firmware,
bootloader và bảng phân vùng. Cần nạp qua TX/RX một lần với thao tác Upload của PlatformIO,
bao gồm bootloader, bảng phân vùng, `ota_data_initial.bin` và ứng dụng.
Không chỉ ghi đè riêng file ứng dụng của bản cũ, vì bảng phân vùng cũ chưa có OTA.
Không dùng Erase Flash; NVS chứa Wi-Fi, MQTT, UID RFID và lịch sử tủ giữ nguyên vị trí.
Do kết nối TX/RX/GND chưa có auto-reset, lần nạp đầu này vẫn cần thao tác BOOT/RESET.

## Những lần sau

1. Build project bằng `pio run -e esp32-s3-devkitc-1`.
2. Đóng cửa tủ, hoàn tất phiên mượn/trả và đợi còi dừng.
3. Khi ESP32 đã kết nối Wi-Fi, mở IP LAN của ESP32 (AP của tủ sẽ tắt).
   Nếu chưa kết nối được Wi-Fi và đang dùng AP cấu hình dự phòng, mở `http://192.168.4.1/`.
4. Trong **Cập nhật firmware qua Wi-Fi**, chọn `releases/firmware.bin`.
   PlatformIO tự chép ứng dụng vào đây sau mỗi lần Build thành công, kể cả khi không có mã nguồn thay đổi.
   Các file có tên phiên bản khác trong `releases` là bản lưu trước đó, không bị ghi đè.
5. Bấm **Cập nhật firmware**, giữ nguồn và kết nối Wi-Fi, đợi ESP32 xác nhận rồi tự khởi động lại.
   Trang tải lại sau 10 giây; nếu Wi-Fi bị ngắt, kết nối lại mạng tủ rồi mở trang.

Chọn file ứng dụng `firmware.bin` được build cho cùng project/board. Không chọn bootloader,
partition table hoặc file flash gộp. Web kiểm tra dung lượng; ESP32 kiểm tra app descriptor,
tên project và xác thực image bằng ESP-IDF trước khi thay đổi vùng khởi động.
Tiến độ upload 100% chưa phải hoàn tất: cần đợi thông báo firmware hợp lệ.
Nếu mất kết nối, kiểm tra tủ và phiên bản đang chạy trước khi bấm lại; trình duyệt không tự upload lại.

## Cách lưu firmware và xử lý lỗi

Giữ nguyên factory ở `0x10000`, dung lượng `0x1f0000`, NVS ở `0x9000`, PHY ở `0xf000`.
Thêm `otadata` ở `0x200000` và hai vùng 3 MiB: `ota_0` ở `0x210000`, `ota_1` ở `0x510000`.
Firmware mới ghi vào vùng OTA không chạy. Upload thiếu, quá hạn 120 giây, lỗi flash hoặc image lỗi
không đổi lựa chọn boot hiện tại. HTTP nhận bị ngắt/quá thời gian socket cũng hủy upload.

Trong lúc cập nhật, task chủ của tủ giữ quyền điều khiển, từ chối lệnh mở cửa, QR,
đổi cấu hình và test thiết bị; không tích lệnh đến trong giai đoạn này để chạy lại sau.
Camera tạm ngừng gửi frame, web chủ động pause để tránh watchdog stream reconnect trong lúc upload.
Sau thất bại, web cho phép thử lại và tiếp tục camera nếu trước đó đang xem.

Bootloader bật rollback. Với bản OTA đang chờ xác nhận, sau tối thiểu 10 giây,
task tủ xác nhận bản mới khi HTTP đã đăng ký thành công và sổ trạng thái tủ không lỗi lưu trữ.
Nếu bản mới reset/crash trước khi xác nhận, lần boot sau sẽ rollback theo cơ chế ESP-IDF.
Đây là kiểm tra startup phần mềm; không xác nhận camera, relay hoặc RFID đã hoạt động điện/cơ thực tế.

API `POST /api/ota` nhận body nhị phân, header `X-ClassHub-Console: 1`, tối đa dung lượng vùng OTA.
`GET /api/device` bổ sung `otaMaxBytes`, `otaActive`, `firmwareSlot` để xem khả năng cập nhật và slot hiện tại.
Giao diện dùng cùng phạm vi truy cập mạng với trang điều khiển hiện tại của tủ.
