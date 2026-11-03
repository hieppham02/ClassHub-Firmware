# RC522: đọc thẻ thiết bị trước khi trả

## Phần đã có

Firmware ESP-IDF có task đọc UID RC522, khởi động sau LCD. Dùng chung SPI2 với
LCD, mỗi thiết bị có CS và tốc độ SPI riêng (RC522 ưu tiên 1 MHz, tự thử 100 kHz
khi không đọc được phiên bản ổn định). Camera và dây LCD
giữ nguyên. Không có đầu đọc vẫn chạy LCD/camera/tủ; đầu đọc thử lại mỗi 5 giây.
GPIO48 dành cho RST RC522, không sử dụng RGB LED trên board cùng lúc.

| Chân RC522 | ESP32-S3 |
| --- | --- |
| 3.3V | 3V3, không cấp 5V |
| GND | GND chung |
| SDA / SS | GPIO47 (CS SPI, không phải SDA I2C) |
| SCK | GPIO42, nối chung SCK LCD |
| MOSI | GPIO41, nối chung SDA/MOSI LCD |
| MISO | GPIO19 (thay GPIO6) |
| RST | GPIO48 |
| IRQ | Không nối |

Ngắt nguồn trước khi đấu dây; giữ dây SPI ngắn. Dùng thẻ/tag ISO14443-A 13.56 MHz
tương thích RC522, quét từng thiết bị một. Nguồn tham khảo giao tiếp/thanh ghi:
[NXP MFRC522 Rev 3.9](https://www.nxp.com/docs/en/data-sheet/MFRC522.pdf).

Sau khi build và nạp firmware, Serial Monitor 115200 hiển thị:

```text
RC522: SPI_PROBE hz=1000000 version=92,92,92 stable=1 transport=ESP_OK RST_GPIO=48 CS_GPIO=47 MISO_GPIO=6
RC522: READY version=0x92 SPI=1000000 Hz CS=47 RST=48; MODE readback passed; present one tag at a time
RC522: RFID_UID=DEADBEEF bytes=4 (read only; not a return verification)
```

UID trên chỉ là ví dụ, version tùy chip. Chép UID thật để gán vào hồ sơ của
**từng thiết bị vật lý**, không chỉ loại thiết bị. UID được in HEX hoa, không
dấu cách; dài 8/14/20 ký tự tương ứng 4/7/10 byte. Không ghi dữ liệu lên thẻ.
Thẻ giữ ổn định trên đầu đọc không liên tục phát sự kiện; sau khoảng 800 ms
không đọc thành công có thể phát lại UID. Bộ đối chiếu phiên phải tự loại trùng.
Nhiều thẻ gây collision hoặc lỗi BCC/CRC thì không phát UID.

Callback `rc522_reader_start(host, callback, context)` là điểm ghép bộ xử lý
phiên sau này. Callback chạy trong task đọc, cần copy UID và enqueue; không xử
lý mạng kéo dài hoặc điều khiển khóa tại đây. Hiện callback cập nhật UID trên web;
có thể đặt tên và lưu/xóa trong flash. Đọc thẻ không mở khóa, không đặt `rfidPassed=true`.

## Chẩn đoán có nguồn nhưng NOT_READY

Đèn nguồn chỉ cho thấy có điện ở mạch LED; không xác nhận IC hoặc đường SPI hoạt động.
Firmware mới đọc VersionReg ba lần liên tiếp ở 1 MHz trong một lượt giữ bus ngắn
để LCD không xen giao dịch. Nếu không ổn định hoặc toàn `00`/`FF`, thử lại ở 100 kHz.
Nếu tốc độ thấp đọc được, giữ tốc độ đó cho việc khởi tạo và quét thẻ.
Sau khởi tạo, đọc lại các bit có thể ghi của ModeReg để xác nhận cả đường ghi và đọc.

Gửi các dòng `SPI_PROBE`, `REGISTER_CHECK` và `READY`/`NOT_READY` để phân tích:

Trang RFID cũng hiển thị mã firmware `rfid-miso19-v5-pullup`, thời gian build và
các mẫu phiên bản ở 1 MHz/100 kHz. Có thể chép dòng **RC522 lần ...** trên web
nếu không mở được Serial. `GET /api/device` có `firmwareId`, `firmwareBuild`
và `rfidProbe`. Serial in `FIRMWARE_ID=...` lúc khởi động.
Nếu chỉ thấy `NOT_READY` nhưng không có `SPI_PROBE`, kiểm tra đúng file firmware
và mã build đang chạy trước khi kết luận lỗi phần cứng.

Bản `rfid-hd-v3-pullup` dùng SPI half-duplex: command phase 8 bit, sau đó
nhận đúng 8 bit dữ liệu (`rx_data[0]`). Ghi cũng dùng command phase 8 bit và
8 bit dữ liệu. CS giữ LOW xuyên suốt hai pha, có thêm thời gian setup/hold,
không chèn dummy clock. Việc này kiểm tra khả năng byte lệnh bị nhận lẫn vào
dữ liệu khi bản cũ báo VersionReg=`EE`, ModeReg=`A2` (đúng các byte lệnh đọc).
Đây là thay đổi cách truyền/nhận để kiểm chứng trên phần cứng, chưa chứng minh
nguyên nhân gốc nếu chưa có kết quả trên board.
Giá trị `EE` không còn được chấp nhận như phiên bản chip; vẫn tiếp tục thử 100 kHz.
Nếu đang thử RST nối thẳng 3V3 và đã tháo dây GPIO48, tắt/bật nguồn để RC522 cũng
được reset khi kiểm tra bản mới; reset riêng ESP32 qua OTA không ngắt nguồn module.

Bản v4 bật rõ `gpio_input_enable(GPIO6)` sau khi thêm thiết bị SPI và hiển thị
mức GPIO6 đọc trực tiếp trong `MISO_INPUT`/web (`rfidProbe.misoLevel`). Khi thử
tháo MISO khỏi RC522 và kéo đầu dây GPIO6 lên 3V3 qua điện trở 1–10 kΩ:
GPIO6=1 nhưng SPI=00 khoanh vùng đường nhận SPI/bộ đệm; GPIO6=0 yêu cầu đo
điện áp ngay chân ESP32 để kiểm tra đường dây/mức điện. Chưa kết luận chip hỏng.

- `00,00,00` hoặc `FF,FF,FF` ở cả hai tốc độ: chưa có phản hồi chip hợp lệ;
  không thể phân biệt chỉ từ log giữa mất tín hiệu MISO, RST, tiếp xúc/nguồn và module lỗi.
- Giá trị thay đổi giữa ba lần đọc: phản hồi không ổn định; kiểm tra chất lượng dây, tiếp xúc và nguồn.
- 1 MHz lỗi, 100 kHz ổn định: giao tiếp có hoạt động ở tốc độ thấp; cần kiểm tra thêm đường tín hiệu.
- Phiên bản ổn định nhưng `REGISTER_CHECK` lỗi: chưa xác minh được ghi/đọc thanh ghi, chưa cho phép quét.

Nếu vẫn lỗi, đo trực tiếp 3V3–GND ở chân module và RST–GND sau reset.
Giữ dây SPI ngắn, kiểm tra mối hàn chân cắm. Khi thử tách LCD để loại trừ ảnh hưởng bus,
ngắt nguồn trước và giữ nguyên các chân SPI firmware đang cấu hình.
Các phép đo/thử module thực tế vẫn cần người dùng thực hiện; kiểm tra host chỉ xác nhận logic driver.

## Luồng cần tích hợp với backend và camera

1. Lưu UID duy nhất cho từng thiết bị vật lý; có luồng gán/đổi thẻ và chống gán
   cùng UID cho hai thiết bị. Không sử dụng UID mẫu làm thiết bị thật.
2. Phiên RETURN cung cấp danh sách các thiết bị và UID cần trả. Payload hiện tại
   chỉ có `equipmentId`, `name`, `quantity`, chưa đủ để biết thẻ nào được phép.
   Cần thống nhất mở rộng hợp đồng với backend trước khi dùng để xác minh.
3. Sau khi mở phiên, LCD yêu cầu quét rồi đặt thiết bị vào tủ. Mỗi UID hợp lệ
   chỉ tính một lần trong phiên; thẻ lạ/sai phiên bị báo lỗi. Hiện số đã quét/
   tổng cần trả và tên thiết bị còn thiếu. Không nhận lượt quét ngoài phiên.
4. Đóng cửa ghi nhận cửa đã đóng, sau đó đối chiếu danh sách. Thiếu lượt quét
   phải báo **chưa quét/thiếu xác nhận RFID**, không suy ra chắc chắn vật chưa
   nằm trong tủ. Camera có thể thấy vật dù người dùng quên quét.
5. Chỉ hoàn tất khi RFID đủ và camera đối chiếu đạt. Camera chưa có kết quả thì
   chờ đối chiếu; đủ lượt quét mà camera thiếu vật thì yêu cầu kiểm tra.
6. Sửa thiếu cần một luồng mở lại có quyền từ backend, liên kết đúng phiên.
   Không tự kích khóa lại từ sự kiện đọc RFID. Reboot cần phục hồi danh sách
   đã quét theo phiên từ lưu trữ; kết thúc/hủy phiên mới xóa danh sách.

Các bước đối chiếu trên là thiết kế, **chưa được triển khai**. Hiện camera chỉ
stream ảnh, chưa nhận diện thiết bị. `CLASSHUB_REQUIRE_RETURN_VERIFICATION`
vẫn là 0: luồng cũ có thể hoàn tất bằng OPENED/CLOSED. Vì vậy chưa dùng bản này
để bắt buộc quét trước khi hoàn tất trả. Phải cập nhật backend cùng bộ đối chiếu
và bật xác minh đồng bộ trước khi áp dụng thực tế.

### Điểm ghép đã kiểm tra trong backend ClassHub

Backend được xác định tại `E:\EAUT\Khoi_nghiep_sang_tao\ClassHub\Backend\ClassHub-API\ClassHub-API`.
Chưa chỉnh sửa backend hoặc database trong đợt thêm đầu đọc này.

- `Models/ThietBi.cs` có `TrackingMode`, `AssetCode`, `so_luong`, chưa có UID.
  Một dòng có thể quản lý nhiều chiếc (BULK), vì vậy không đặt một UID duy nhất
  lên cả dòng. Cần bảng gán thẻ cho từng chiếc, tham chiếu `ThietBi.id`.
- `Services/QrAccessService.cs`, hai đường `Open`/`Retry` đang gửi
  `expectedEquipment` từ `phieu_muon_thiet_bi`. Cần gửi cùng danh sách thẻ đã
  được chốt cho phiên mượn, không tự lấy lại toàn bộ thẻ hiện có của loại đó
  mỗi lần retry.
- Cũng tại service này, `RequireReturnVerification=false` nên `CLOSED` đang
  hoàn tất phiếu. Cần bật đồng bộ với firmware sau khi có kết quả RFID/camera
  thật và luồng sửa thiếu/mở lại. Chưa bật cờ chỉ vì đã đọc được UID.
- `Models/QrAccess.cs` đã có `RfidPassed`, `CameraPassed`, `VerificationSequence`:
  có thể tiếp tục dùng cho kết quả tổng hợp; danh sách thiếu cần dữ liệu chi tiết
  bổ sung để frontend hiển thị đúng thiết bị.

## Kiểm tra

`tests/run-host.ps1` kiểm tra CRC, UID nhiều cascade, dữ liệu lỗi và xung đột
GPIO cùng các kiểm tra tủ/camera có sẵn. Build target kiểm tra API ESP-IDF.
Chưa thay thế kiểm tra mạch thật: cần thử không gắn module, quét/giữ/rút thẻ,
quét thẻ 4/7/10 byte, nhiều thẻ, mất kết nối và LCD/camera hoạt động đồng thời.

## Chuyển MISO sang GPIO19

Bản v5 đổi riêng MISO từ GPIO6 sang GPIO19. Ngắt nguồn, bỏ dây thử kéo lên 3V3,
chuyển dây MISO RC522 sang GPIO19; SCK42, MOSI41, CS47 giữ nguyên.
GPIO19 vốn là USB D-; cấu hình này dùng cổng UART ngoài TX/RX/GND để nạp và log,
không dùng cổng USB native D-/D+ đồng thời với RFID. GPIO43/44 của UART giữ nguyên.
Các đoạn chẩn đoán GPIO6 phía trên mô tả bản cũ; bản v5 in và hiển thị đúng GPIO19.
Nếu đang thử RST nối trực tiếp 3V3, giữ dây GPIO48 tháo khỏi chân RST module.
Việc đổi chân là phép kiểm chứng, chưa xác nhận GPIO6 bị hỏng.

## Bản v6: lỗi sau khi READY

Khi `readerReady=true` nhưng báo `READ_FAILED`, đường khởi tạo thanh ghi đã qua,
chưa có nghĩa là quét UID thành công. Bản `rfid-rf-v6-pullup` ghi thêm `stage`,
`IRQ`, `ERROR`, `FIFO`, `valid_bits` để phân biệt WUPA, ANTICOLL, SELECT, SAK_CHECK và HALT.
Các phản hồi WUPA/anticollision không có CRC_A; SAK được kiểm CRC_A bằng phần mềm.
Vì RxMode tắt kiểm CRC phần cứng, bỏ chặn riêng cờ CRCErr trong exchange; vẫn chặn
collision, parity, protocol, overflow, thermal và write error. CRC sai trong SAK
vẫn bị bộ kiểm UID từ chối. Chưa có log chi tiết thì không khẳng định cờ CRC là
nguyên nhân của một lỗi READ_FAILED cụ thể.
