# Danh sách tài sản phòng cho bản thử AI

Đã thêm cấu hình mẫu vào ba phòng `DTD201`, `EAUT101`, `EAUT105`. Chưa nhân bản
sang toàn bộ phòng trong hệ thống. Cơ sở là danh sách chép từ ảnh người dùng
cung cấp và yêu cầu bổ sung một TV TCL 4K, ước khoảng 80 inch.

Mã đợt: `d0ad8a80-7894-5783-9617-7d5aba76f693`.

| Tài sản | Số lượng mỗi phòng | Đơn vị |
| --- | ---: | --- |
| Máy chiếu Sony | 1 | bộ |
| Bảng viết phấn | 1 | cái |
| Camera Hikvision | 1 | cái |
| Điều hòa Casper | 1 | cái |
| Ghế inox chân quỳ lưới đen | 1 | cái |
| Đèn Sino | 10 | bộ |
| Bàn giảng viên | 1 | cái |
| Ghế gấp inox xanh | 76 | cái |
| Bàn gỗ 0,5 m x 1,2 m | 38 | cái |
| Quạt cây Midea | 1 | cái |
| Bộ phát Wi-Fi Grandstream | 1 | cái |
| Quạt trần Nanoco | 2 | cái |
| Đèn LED 1,2 m bán nguyệt | 3 | cái |
| TV TCL 4K | 1 | cái |

Mỗi phòng 14 dòng, tổng số lượng 138 theo đơn vị tương ứng; không cộng số bộ
đèn thành số bóng vì chưa rõ số bóng/bộ. Có 9 loại dùng điện và 5 loại nội thất.
Toàn đợt thêm 42 dòng tài sản, 14 danh mục ROOM và 42 nhật ký nhập cấu hình.

## Phân biệt với kiểm kê thực tế

- Tất cả dòng mới có ghi chú `MOCK`, số lượng và tình trạng chưa kiểm kê tại
  phòng. Bảng `room_equipment_profile` ghi `TEMPLATE_UNVERIFIED`,
  `quantity_verified_in_room=false`, `condition_verified=false`.
- Tên phòng trong ảnh chưa được xác nhận, nên đây là mẫu áp dụng có chủ đích
  cho ba phòng thử nghiệm, không phải kết luận cả ba có đúng thiết bị như ảnh.
- `condition=TOT` là trạng thái giả định theo quy ước ứng dụng hiện có;
  không khẳng định đã kiểm tra tình trạng thiết bị.
- `usage_scope=ROOM`, `tracking_mode=BULK`; không thay đổi đồ dùng trong tủ
  (`CABINET`), không thêm chúng vào danh sách RFID nhận/trả.
- Đèn Sino: loại bóng trong ảnh chưa rõ. TV: 80 inch chỉ là ước lượng.
- Model chi tiết, công suất điện vào và nhánh đo để NULL; chưa xác nhận thiết
  bị nào nằm trên nhánh ZMCT103C. Không suy ra công suất từ số inch hoặc thương hiệu.
- Không ghi giảm kho hay tạo giao dịch bàn giao vật lý giả. Nhật ký dùng hành
  động `ASSET_BASELINE_IMPORT`, nêu rõ đây là nhập cấu hình tài sản giả định.

## Cấu trúc và kiểm tra

Dùng `equipment_catalog` và `equipment` sẵn có để giao diện quản lý tài sản
hiện tại đọc được danh sách mới. Bảng phụ `room_equipment_profile` bổ sung hãng,
đơn vị, nguồn, ước lượng kích thước và các trường thông số/nhánh đo còn trống;
backend chưa có API chỉnh sửa riêng cho các trường phụ này.

Chương trình `tools/ai_data/room_inventory.py` mặc định chỉ chuẩn bị SQL; thêm
`--apply` mới ghi DB. Nhập toàn đợt trong một transaction, dùng batch ID cố định
để chạy lại không sinh trùng. DDL tạo bảng phụ commit riêng theo MariaDB.

Đã kiểm tra từng phòng, tên, số lượng, phạm vi ROOM, nguồn MOCK, công suất NULL,
TV 80 inch chưa xác nhận, liên kết danh mục, đủ 42 nhật ký. So sánh bản chốt
trước/sau xác nhận 23 dòng thiết bị và 16 danh mục cũ không đổi; số phiên mượn,
thiết bị của phiên, mẫu cảm biến, node và tủ không đổi.

Các file SQL và JSON làm việc ở `.codex-temp/ai-data/inventory/`; bản bàn giao
lưu ở `artifacts/classhub-room-inventory/`. `rollback.sql` chỉ dành để gỡ đợt này
sau khi kiểm tra các thay đổi tài sản phát sinh về sau; chưa chạy file đó.

**Chưa tính lại 60.390 mẫu dòng điện đã tạo trước đây theo danh sách này.**
Bộ sinh cũ vẫn dùng hồ sơ dòng điện giả định, không tự trở thành mô hình điện
theo từng thiết bị sau khi bổ sung danh mục. Bước kế tiếp cần định nghĩa các
trạng thái tải và thông số giả định riêng (không ghi giả vào trường công suất
định mức), rồi tạo một đợt dữ liệu mới có liên kết với bản chốt danh mục này.
