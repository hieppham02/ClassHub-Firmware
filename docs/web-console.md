# Web thiết bị ClassHub

Nếu chưa có Wi-Fi đã lưu, ESP32 bật AP cấu hình; kết nối Wi-Fi của tủ rồi mở `http://192.168.4.1/`.
Nếu đã lưu Wi-Fi, ESP32 khởi động ở chế độ STA và thử kết nối trước, chưa phát AP.
Sau 30 giây không có IP LAN, ESP32 bật AP dự phòng để cấu hình lại và tiếp tục thử Wi-Fi mỗi 15 giây.
Khi nhận được IP từ router, AP tắt. Kết nối máy tính/điện thoại cùng mạng LAN rồi mở IP ESP32
được in trong Serial (`Camera on LAN: http://.../`) hoặc xem trong danh sách thiết bị của router.
Mất kết nối ngắn sẽ được thử lại; mất kết nối kéo dài 30 giây sẽ bật lại AP cấu hình.
AP đã bật sẽ giữ nguyên trong lúc lưu/thử cấu hình mới cho tới khi nhận IP thành công.
Khi OTA đang chạy, việc đổi chế độ Wi-Fi được hoãn để giữ kết nối upload.
Trang `/setup` tiếp tục dùng để cấu hình Wi-Fi, MQTT và mã phòng.

## RFID

Đưa một thẻ vào RC522, bấm **Chọn mã vừa quét**, nhập tên thiết bị rồi bấm **Lưu RFID**.
Mã đã chọn giữ nguyên khi có thẻ khác được quét, để tránh lưu nhầm thiết bị.
Danh sách tối đa 32 UID (4, 7 hoặc 10 byte), mỗi tên tối đa 48 byte UTF-8.
Lưu lại cùng UID sẽ cập nhật tên. Có thể xóa từng mã và tải danh sách JSON.

Danh sách được lưu vào NVS `classhub-rfid`, khóa `tags`, trên ESP32 và tồn tại sau khởi động lại.
Chỉ ghi flash khi bấm lưu/xóa; việc quét liên tục không ghi flash.
Bộ nhớ lỗi không bị tự động xóa. Danh sách này phục vụ đăng ký thiết bị;
việc quét chưa tự đánh dấu xác minh trả thiết bị trong backend.
Trạng thái RC522 sẵn sàng dựa trên khởi tạo và đọc thanh ghi của module;
cần quét thẻ thực tế để xác nhận đầy đủ đường RF.

## Camera và ảnh cho computer vision

WebSocket `/ws` mặc định dùng JPEG chất lượng 70 ở độ phân giải VGA 640×480,
không thu nhỏ ảnh. Có thể chọn **RGB565 · không nén** để nhận nguyên pixel nguồn.
Mã hóa JPEG chạy ở task riêng trên core 1; camera chỉ chép frame vào một slot PSRAM
rồi trả framebuffer ngay, không chờ mạng hoặc encoder.
Trang hiển thị FPS capture và FPS nhận riêng. Luồng có điều tiết theo tốc độ nhận,
mỗi lần một trình duyệt xem; khi đường truyền chậm, gửi khung mới nhất thay vì tích hàng đợi.
Không thể bảo đảm FPS nhận luôn bằng FPS capture nếu Wi-Fi/trình duyệt không đủ nhanh.
Một khung RGB565 có 614.400 byte dữ liệu; 12,5 FPS cần khoảng 61,44 Mbps chưa tính
overhead mạng. JPEG giảm dữ liệu truyền; web hiển thị dung lượng/định dạng frame
để đo thực tế. FPS JPEG vẫn phụ thuộc thời gian mã hóa và đường truyền, không cam kết
bằng FPS sensor. Nếu encoder lỗi hoặc vượt dung lượng slot thì gửi RGB565 thay thế.

**Chụp PNG gốc** yêu cầu một frame RGB565 mới riêng, rồi tạo PNG ở trình duyệt,
giữ kích thước gốc và không lấy từ ảnh JPEG đã nén. Chụp được cả khi pause phần xem trước.
Các bộ lọc sáng/tương phản/bão hòa trên web chỉ ảnh hưởng phần xem trước.
Thiết lập phơi sáng/gain/cân bằng trắng/lật ảnh của cảm biến ảnh hưởng cả ảnh được chụp.
Tên PNG có kích thước, số thứ tự frame và thời gian chụp trên máy tính.

Giao thức vẫn có header 16 byte `OVF1`. Byte 14: 0 = RGB565, 1 = JPEG,
2 = RGB565 trả lời yêu cầu chụp gốc. Header width/height là kích thước nguồn.
Client gửi `FORMAT JPEG` hoặc `FORMAT RAW`, `NEXT` để cấp quyền gửi một frame,
`CAPTURE` để yêu cầu một ảnh RGB565. Trình duyệt chỉ gửi NEXT sau khi giải mã và vẽ
xong JPEG để tránh tích frame; snapshot được ưu tiên cho lượt frame tiếp theo.
Trình duyệt thiếu `createImageBitmap` tự dùng RGB565. Mỗi phiên giữ một raw slot
614.400 byte và một output slot 614.416 byte, giới hạn một frame đang xử lý/gửi.

## Test

- Mở cửa: phát xung relay 1.500 ms; GPIO2 dùng pull-up, công tắc kéo xuống GND khi đóng cửa.
  HIGH = cửa mở, LOW = cửa đóng; cần mở rồi đóng, mỗi mức ổn định hơn 50 ms để hoàn tất chu kỳ.
- Còi: phát âm 300 ms, không chặn task điều khiển.
- MQTT: gửi QoS 1 tới `iot/cabinet/{room}/diagnostics`, chờ PUBACK đúng message ID và phiên kết nối, tối đa 10 giây.
  Thành công xác nhận broker đã nhận bản tin; không xác nhận ứng dụng backend xử lý nó.

Lệnh mở cửa/còi bị chặn khi tủ đang bận hoặc hiển thị QR còn hạn.
Lệnh nằm trong hàng đợi quá 3 giây bị từ chối. Trình duyệt không tự gửi lại lệnh vật lý khi mất mạng.
Phản hồi test cửa/còi xác nhận firmware bắt đầu phát xung; cần quan sát phần cứng để xác nhận tác động.

## Phát triển và kiểm tra

Sửa giao diện trong `src/web_console.html`, sau đó chạy `py tools/embed_web.py`
để cập nhật `src/web_console.h` được biên dịch vào firmware.
`powershell -ExecutionPolicy Bypass -File tests/run-host.ps1` kiểm tra UID,
luồng mượn/trả, test thiết bị, giao thức stream, giao diện và độ đồng bộ của asset nhúng.
Kiểm tra host không thay thế việc nạp và thử trên ESP32-S3, RC522, camera, relay và còi thực tế.
