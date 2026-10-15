# QR firmware – ESP32-S3


## Chế độ kiểm thử hiện tại

CLASSHUB_REQUIRE_RETURN_VERIFICATION=0 trong QrAccess.h: đóng cửa trả đồ sẽ giải phóng tủ, không chờ hoặc gửi VERIFIED. Backend RequireReturnVerification=false hoàn tất khi nhận OPENED rồi CLOSED đúng lệnh. Không giả lập RFID/camera đạt. Bật lại cả hai công tắc khi nối cảm biến thực tế; các mô tả cảm biến dưới đây dành cho chế độ đầy đủ.

Host test mặc định có 19 kiểm tra đóng/mở; build host test với -DCLASSHUB_REQUIRE_RETURN_VERIFICATION=1 để chạy 34 kiểm tra chế độ cảm biến.

## Tương thích

Mượn trả của tủ phòng và tủ tự do chỉ dùng QR. Firmware subscribe QR/action và publish status/heartbeat/qr-result. Debounce và chu kỳ heartbeat giữ nguyên. Message QR thiếu/sai dữ liệu không được chuyển thành lệnh admin. Trong lúc QR đang mở hoặc chờ kiểm tra trả, lệnh admin không được chen vào điều khiển khóa.

## Khóa điện và nút đóng cửa

Đã bỏ servo/PWM. GPIO 4 (`LATCH_PIN`) điều khiển ON/OFF: HIGH bật khóa điện để mở, LOW tắt/khóa; boot đặt OFF, nhận lệnh mở đặt ON, xác nhận đóng cửa đặt OFF. Không tự thêm timer tắt khóa. GPIO 5 (`CLOSE_BUTTON_PIN`) dùng INPUT_PULLDOWN: nút nối GPIO 5 với 3.3V, chạm/nhấn lên HIGH ổn định trên 50 ms xác nhận cửa đóng. Có thể thay nút bằng công tắc hành trình cùng quy ước mức điện. GPIO 4 là tín hiệu điều khiển driver/relay khóa điện. LED GPIO 35 và nút cấu hình WiFi GPIO 15 giữ nguyên.

Khi nút đóng cửa kích hoạt trong lượt mở: tắt GPIO 4 và gửi status LOCKED/isOpen=false; với phiên QR gửi thêm CLOSED cùng sessionId/commandId. Trả đồ vẫn cần RFID/camera đạt, không tự hoàn tất chỉ vì nhấn nút.

Firmware subscribe thêm `backend/cabinet/{roomId}/qr`, hiển thị nguyên `content` thành QR trên LVGL, xóa khi hết hạn/nhận lệnh mở. Dùng [QR của LVGL](https://lvgl.io/docs/open/9.2/libs/qrcode), không thêm thư viện. Buffer MQTT tăng từ 256 lên 8192 byte để nhận danh sách thiết bị; tối đa 64 loại thiết bị và toàn bộ message phải vừa buffer. Giữ cấu hình MQTT đang lưu; không sửa thông tin kết nối.

Thiết bị đồng bộ giờ UTC qua NTP. Nếu giờ chưa đồng bộ, từ chối QR/lệnh mở mới; đợi NTP rồi cấp lại QR trên web. QR tối đa 180 giây, lệnh mở tối đa 30 giây. Heartbeat và lệnh bảo trì admin vẫn hoạt động khi NTP chưa sẵn sàng.

## Phản hồi và chống lặp

Lệnh QR `action=open` phải có `sessionId`, `room`, `purpose=RECEIVE|RETURN`, `commandId` 32 ký tự hex, `expiresAt` UTC và `expectedEquipment`. Firmware ghi nhận lệnh vào NVS trước khi bật khóa điện. Cùng mã lệnh chỉ phát lại kết quả, không mở lần hai; áp dụng cả reconnect và reboot. Khác mã lệnh trong lúc mở bị từ chối. Sau kiểm tra trả thất bại, cho phép QR RETURN mới cùng phiếu.

Publish QoS 1, không retained vào `iot/cabinet/{roomId}/qr-result`:

```json
{"sessionId":123,"commandId":"0123456789abcdef0123456789abcdef","event":"OPENED"}
{"sessionId":123,"commandId":"0123456789abcdef0123456789abcdef","event":"CLOSED"}
{"sessionId":123,"commandId":"0123456789abcdef0123456789abcdef","event":"VERIFIED","rfidPassed":true,"cameraPassed":true,"sequence":1}
```

`OPENED` xác nhận đã bật đầu ra khóa điện; không có cảm biến riêng để chứng minh chốt đã mở cơ khí. `CLOSED` chỉ được ghi nhận sau nhánh khóa do nút/công tắc cửa GPIO 5 (HIGH, debounce 50 ms). RFID/camera không được tự suy ra từ đóng cửa. Mất MQTT: giữ kết quả và thử gửi lại, sau reconnect phát lại OPENED → CLOSED → VERIFIED theo thứ tự. Backend xử lý các sự kiện lặp.

Boot đặt khóa điện OFF. Lệnh bị gián đoạn trước khi ghi OPENED chuyển OPEN_FAILED, không tự mở lại. Nếu đã ghi OPENED, phát lại OPENED và đợi nút/công tắc xác nhận đóng cửa; không tự kết luận hoàn tất khi boot. Cần kiểm tra vật lý sau reset giữa thao tác.

NVS namespace `classhub-qr` lưu biên nhận của từng mã lệnh và context hiện tại; không xóa tự động để tránh mở lại lệnh cũ. Hết dung lượng/lỗi ghi/collision khóa NVS: từ chối mở bằng QR. Bảo trì/xóa ledger chỉ khi các phiếu đã đối soát, backend không còn lệnh có thể retry; không xóa cấu hình MQTT/WiFi. Đây là giới hạn dung lượng cần theo dõi khi vận hành dài hạn.

## Nối RFID/camera thật

Repo hiện chưa có driver RFID hoặc camera. `include/QrAccess.h` cung cấp context và điểm nối; chạy trên foreground task, task cảm biến/ISR phải đưa kết quả qua queue:

```cpp
String sessionId, commandId, expected;
if (qrVerificationContext(sessionId, commandId, expected)) {
    // Lưu sessionId/commandId cho lần kiểm tra này.
    // Parse expected: [{equipmentId,name,quantity}, ...].
    // Quét RFID và đối chiếu đầy đủ equipmentId/quantity; chạy camera thật.
    // Khi có cả hai kết quả thực tế, gọi trên foreground task:
    submitQrVerification(sessionId, commandId, actualRfidPassed, actualCameraPassed);
}
```

API từ chối context khác/đã supersede/phiếu RECEIVE và không cho kết quả trễ làm lùi trạng thái trả đã đạt + đóng cửa. Mỗi lần kết quả hợp lệ tăng sequence và lưu trước khi publish. Thiếu một kết quả thì chưa gọi API; không mặc định true. Backend chỉ hoàn tất khi RFID + camera đạt và đã CLOSED. Không có đường Serial/MQTT công khai để giả lập kết quả cảm biến.

## Kiểm tra và nạp

Build: `pio run -e esp32-s3-devkitc-1`. Không tự flash thiết bị.
Đã qua 29 kiểm tra `test/qr-rules.cpp` (dữ liệu/thời hạn UTC/điều kiện giữ tủ) và 34 kiểm tra `test/qr-runtime.cpp` chạy chính adapter firmware với stub màn hình, NVS và transport, dùng ArduinoJson thực tế. Có kiểm tra chống lặp qua reset, gián đoạn trước/sau mở, lỗi NVS, mất mạng, tương quan RFID/camera và hiển thị/hết hạn QR. Host test không kiểm chứng mức điện trên chân khóa/nút. Code WiFi giữ nguyên.

Chạy host test với g++ từ thư mục IOT, dùng thư viện ArduinoJson đã được PlatformIO resolve:

```powershell
g++ -std=c++11 -Wall -Wextra -Werror -I include test/qr-rules.cpp -o .pio/qr-rules.exe
.pio/qr-rules.exe
g++ -std=c++11 -Wall -Wextra -Werror -I test/stubs -I include -I .pio/libdeps/esp32-s3-devkitc-1/ArduinoJson/src test/qr-runtime.cpp -o .pio/qr-runtime.exe
.pio/qr-runtime.exe
```

Kiểm tra phần cứng cần thực hiện riêng: quét QR màn hình 128×160 trên điện thoại, mở/đóng, ngắt mạng, reset giữa thao tác, RFID thiếu/đủ và camera sai/đạt. Host test/build không chứng minh cảm biến hoặc cơ khí hoạt động.

