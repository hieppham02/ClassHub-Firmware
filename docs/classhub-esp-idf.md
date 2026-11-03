# ClassHub + OV7670 trên ESP32-S3

Project này chạy hoàn toàn bằng ESP-IDF (PlatformIO), không dùng Arduino,
WiFiManager, LVGL hay TFT_eSPI. Camera dùng driver esp32-camera đã có trong
project; LCD dùng driver ST7735 SPI hiện tại. MQTT, JSON và QR dùng các component
`espressif/mqtt`, `espressif/cjson`, `espressif/qrcode`.

## Sơ đồ chân

Dùng sơ đồ chân camera và LCD từ `../testxxxx/include/test_config.h`:

| OV7670 | GPIO |
| --- | ---: |
| SIOD/SDA | 9 |
| SIOC/SCL | 10 |
| XCLK | 11 |
| D0, D1, D2, D3 | 4, 5, **8**, 7 |
| D4, D5, D6, D7 | 15, 16, 17, 18 |
| VSYNC, HREF, PCLK | 14, 12, 13 |
| PWDN, RESET | Nối LOW, HIGH bên ngoài; GPIO = -1 |

| LCD ST7735 | GPIO |
| --- | ---: |
| CS, RST, DC/A0 | 38, 39, 40 |
| MOSI/SDA, SCK | 41, 42 |
| BL | Cấp nguồn bên ngoài |

Chuyển dây điều khiển tủ theo bảng sau. Khai báo tại `src/classhub.h`:

| Chức năng | GPIO cũ IOT | GPIO mới | Quy ước |
| --- | ---: | ---: | --- |
| Khóa điện/driver | 4 | **1** | HIGH mở, LOW tắt; boot LOW |
| Nút/công tắc cửa | 5 | **2** | INPUT_PULLUP; nối với GND khi đóng, LOW = đóng |
| Buzzer | 6 | **21** | 1000 Hz; LEDC timer/channel 1 |
| LED | 35 | Không sử dụng | Firmware không cấu hình hoặc ghi GPIO LED |
| Cấu hình mạng | 15 | **0** | Nút BOOT hoặc nút nối GND; active LOW |

GPIO0 đổi sang active LOW theo nút BOOT: giữ 3 giây **sau khi board đã boot**.
Nút cũ nối 3.3V ở GPIO15 phải đổi dây theo quy ước trên. GPIO19 hiện dùng MISO RC522 (thay GPIO6), GPIO20 chưa dùng; USB native không dùng đồng thời với RFID;
không dùng GPIO flash/PSRAM cho các đầu ra mới. Khóa điện nối qua driver/relay.

## Khởi động và cấu hình

Tên AP là ID tủ trong trường `roomId`; mật khẩu là `<ID tủ>123@@`.
Ví dụ: `DTD201` / `DTD201123@@`. ID gồm 3-20 ký tự chữ, số, `-` hoặc `_`.
Khi AP đang bật và lưu ID mới, AP đổi tên; cần kết nối lại theo ID mới.

Chưa có Wi-Fi đã lưu thì AP bật ngay. Nếu đã lưu Wi-Fi, khởi động bằng STA và thử
kết nối trước; sau 30 giây chưa nhận IP thì bật AP dự phòng. Khi nhận IP LAN,
AP tắt. STA thử lại cách nhau 15 giây khi thất bại để giữ AP dự phòng hoạt động ổn định.
Serial có dòng `AP_READY` khi AP đã bật và `Camera on LAN: http://.../` khi nhận IP.
HTTP khởi động trước LCD/camera; nếu không cấp phát được bộ đệm PSRAM cho
camera web, `/setup` vẫn hoạt động và chỉ tắt streaming.

1. Kết nối AP **<ID tu>**, mật khẩu **<ID tu>123@@**.
2. Lần đầu mở `http://192.168.4.1/setup`. Nhập SSID/mật khẩu WiFi có Internet,
   MQTT host/port/username/password và mã phòng. Thông số MQTT mặc định của project
   IOT cũ đã được chép vào `src/classhub_defaults.h` cục bộ; trang cấu hình điền sẵn
   host, port, username, mã phòng và không trả mật khẩu về trình duyệt.
3. Để trống mật khẩu để giữ giá trị đang lưu. Chọn "WiFi không có mật khẩu" nếu
   chuyển sang mạng mở. Sau khi gửi, xem LCD để xác nhận hoặc sửa dữ liệu sai.
4. Khi Wi-Fi kết nối thành công, kết nối máy tính/điện thoại vào cùng mạng LAN,
   mở IP ESP32 được in trong Serial hoặc xem danh sách thiết bị của router.
   Trang `/` có camera, RFID, test thiết bị và OTA; AP đã tắt khi STA nhận IP.
5. Truy cập trực tiếp /setup bất kỳ lúc nào, kể cả sau khi lưu hoặc khởi động lại;
   không cần giữ BOOT/GPIO0. Chỉ áp dụng cấu hình khi tủ rảnh, đã hoàn tất phiên đóng cửa.

MQTT dùng TLS với bundle CA và kiểm tra hostname. NTP UTC phải sẵn sàng trước khi
kết nối TLS và nhận QR mới. Camera/AP/LCD vẫn chạy khi chưa có Internet.

NTP chỉ bắt đầu sau khi STA nhận IP, có hai server và không chờ ngẫu nhiên 0-5
giây lúc khởi động. Chỉ báo MQTT online khi đã nhận SUBACK thành công cho cả
`action` và `qr`; nếu subscribe lỗi/chưa có ACK, thử lại sau 3 giây. Phiên MQTT
persistent dùng client ID theo tủ + MAC, giữ message QoS1 khi reconnect. Lệnh
được xếp hàng vẫn phải qua kiểm tra thời hạn và chống mở lặp trước khi thực thi.

Thời hạn danh nghĩa vẫn là QR 180 giây/lệnh mở 30 giây. Cho phép đồng hồ ESP32
chậm hơn server tối đa 5 giây ở giới hạn thời gian còn lại; không cho phép nhận
lệnh đã hết hạn theo đồng hồ hiện tại. Quy tắc cũ quá sát giới hạn làm mã mới
bị từ chối khi hai đồng hồ lệch chỉ vài giây.

Serial có các mốc `WIFI_GOT_IP`, `TIME_SYNC`, `MQTT_START`,
`MQTT_LINK_CONNECTED`, `MQTT_SUBACK`, `MQTT_READY` để đo chỗ chờ. Có log
`MQTT_RX`, `QR_DISPLAY`, `OPEN_EXECUTED` và lý do `QR_REJECT`/`OPEN_REJECT`
để phân biệt lỗi nhận gói với lỗi xác thực. Không in token QR hoặc mật khẩu.

Cấu hình mới được lưu nguyên khối trong NVS `classhub-idf/config`. Nếu chưa có,
firmware đọc cấu hình cũ `classhub-mqtt/config`; WiFi STA cũ cũng được đọc từ
driver nếu chưa có cấu hình mới. Không tự xóa NVS. File `classhub_defaults.h`
được gitignore; khi chuyển máy có thể dùng `/setup` hoặc chép file example.

## Luồng điều khiển

`main.c` sở hữu camera/SCCB và WebSocket camera. Task `cabinet_lcd` sở hữu duy
nhất LCD: hiển thị QR có quiet zone hoặc trạng thái tủ/mạng. Task `classhub` sở
hữu GPIO, NVS, xử lý QR, cấu hình và yêu cầu publish. Callback MQTT chỉ ghép các
mảnh message tối đa 8192 byte rồi đưa vào queue; không điều khiển khóa từ callback.

Camera RGB565 **640×480 (VGA)**, mức tối đa của OV7670; XCLK 10 MHz, CLKRC 0x01,
HSTOP 14 và cửa sổ VGA gốc của driver. D2 vẫn GPIO8. Board **N16R8** dùng flash
16 MB và PSRAM octal 8 MB ở 80 MHz. Hai framebuffer cùng buffer web nằm trong
PSRAM: mỗi frame 614400 byte, gói web 614416 byte. Web nhận ảnh VGA thật,
trần truyền 2 FPS; tốc độ thực tế phụ thuộc Wi-Fi và thời gian chụp. GPIO35–37
thuộc bus PSRAM và không dùng làm GPIO đầu ra. Firmware không sử dụng LED.
Màn hình LCD chỉ hiển thị QR/trạng thái; ảnh camera truyền lên web.
HTTP và task tủ khởi động trước camera nên lỗi khởi tạo camera không chặn cấu hình
hay xử lý cửa. `CAMERA_STARTUP_DIAGNOSTICS=1` trong main.c bật lại kiểm tra mở rộng.

Topic theo mã phòng:

| Subscribe | Publish |
| --- | --- |
| `backend/cabinet/{room}/action` | `iot/cabinet/{room}/status` |
| `backend/cabinet/{room}/qr` | `iot/cabinet/{room}/heartbeat` |
| | `iot/cabinet/{room}/qr-result` |

Giữ hợp đồng backend cũ: QR display tối đa 180 giây; lệnh open tối đa 30 giây;
sessionId dương, commandId 32 ký tự hex, purpose RECEIVE/RETURN, danh sách tối đa
64 loại thiết bị. Payload sai dạng QR không chuyển thành lệnh admin. Admin không
chen vào phiên QR đang giữ tủ. Heartbeat retained mỗi 15 giây; status và qr-result
QoS1 không retained. Reconnect phát lại OPENED → CLOSED → VERIFIED nếu có.

Khóa điện được cấp HIGH **1500 ms** rồi LOW, giữ đúng main.cpp IOT thực tế (tài
liệu IOT cũ mô tả chưa đúng chỗ này). Hết xung không đồng nghĩa cửa đã đóng:
isOpen vẫn true cho đến cạnh LOW của công tắc cửa ổn định **hơn 50 ms**. Khi đó
firmware gửi LOCKED/isOpen=false và CLOSED của đúng lệnh QR.

Nút/công tắc cửa hoạt động như limit switch: GPIO2 dùng pull-up (cả pull-up nội được bật),
công tắc nối GPIO2 xuống GND khi cửa đóng. Cửa đóng giữ LOW, cửa mở nhả về HIGH.
Mỗi lượt mở mới phải quan sát HIGH ổn định hơn 50 ms, sau đó LOW ổn định hơn 50 ms
mới xác nhận CLOSED. LOW giữ từ trước khi kích khóa không hoàn tất phiên; giữ LOW
sau khi đóng không lặp lại sự kiện.
Sau reboot giữa phiên OPENED, cần quan sát lại chu kỳ nhả → ấn giữ để xác nhận.

NVS `classhub-qr` giữ biên nhận và command đang active tương thích dữ liệu cũ.
Lệnh được lưu trước khi cấp điện, chống mở lặp kể cả reboot; lỗi ghi, dữ liệu hỏng
hay collision key đều chặn mở QR. Boot không tự mở lại: ACCEPTED chuyển
OPEN_FAILED; OPENED phát lại và đợi công tắc xác nhận. Không tự xóa các biên nhận;
NVS đầy sẽ từ chối QR mới, cần bảo trì sau khi đối soát với backend.

Giữ `CLASSHUB_REQUIRE_RETURN_VERIFICATION=0` như project IOT: RETURN hoàn tất
bằng OPENED/CLOSED, không giả lập kết quả RFID/camera. Camera web chưa làm nhận
diện đồ vật. `classhub_submit_verification()` dành cho pipeline cảm biến thật,
không có API HTTP/MQTT để giả lập. Khi bật kiểm tra phải bật tương ứng ở backend.

## Build và kiểm tra

```powershell
& 'D:/.platformio/penv/Scripts/pio.exe' run -e esp32-s3-devkitc-1
powershell -ExecutionPolicy Bypass -File tests/run-host.ps1
```

Phân vùng factory `0x10000..0x200000` đủ cho firmware MQTT/TLS, thay phân vùng
app 1 MB cũ. Flash target 16 MB theo N16R8, giữ nguyên layout phân vùng; NVS giữ
offset `0x9000`, size `0x6000`; PHY vẫn `0xf000`. Không có OTA trong layout này.
Build không tự flash/reset board.

Khi chẩn đoán reset lúc mở tủ: `rst:0x8 (TG1WDT_SYS_RST)` cho biết watchdog
Timer Group 1 đã reset chip; riêng mã này chưa xác định được hàm gây kẹt.
Interrupt watchdog dùng 800 ms theo mặc định ESP-IDF khi bật PSRAM.
`ACTUATOR_STAGE` ghi từng bước mở GPIO1, xóa QR, cập nhật LCD, bật buzzer và
gửi MQTT status. `RESET_DIAG` lúc boot in nguyên nhân reset và bước cuối được
giữ trong RTC RAM, không ghi flash; bước này chỉ là dấu vết, chưa kết luận thủ phạm.
Nếu vẫn reset, lấy log từ MQTT_RX đến RESET_DIAG của lần boot kế tiếp.

Host checks chạy quy tắc QR và logic runtime thực tế với fake GPIO/NVS/transport;
không chứng minh mức điện, độ ổn định nguồn, TLS thực tế hoặc khả năng quét QR.
Cần thử trên board: mở camera web đồng thời quét QR LCD, WiFi/MQTT reconnect,
khóa 1.5 giây, đóng cửa/debounce và reboot giữa phiên. Chỉ nạp firmware khi đã
đổi dây khóa/nút/buzzer theo GPIO mới.

Tài liệu component chính thức:
[ESP-MQTT](https://github.com/espressif/esp-mqtt),
[QR code](https://components.espressif.com/components/espressif/qrcode),
[thay đổi JSON ở IDF 6](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/migration-guides/release-6.x/6.0/protocols.html).

Camera web: socket gửi hết payload trước khi báo thành công; lỗi/timeout đóng socket để trình duyệt kết nối lại. Trang thử NEXT lại sau 1,5 giây không có ảnh và kết nối lại sau 8 giây. Trạng thái chờ ảnh được hiển thị riêng với trạng thái đang nhận hình. HTTP dùng 5 socket client, LRU dọn kết nối cũ; tổng LWIP 16 socket để có chỗ cho MQTT/NTP/DNS.
