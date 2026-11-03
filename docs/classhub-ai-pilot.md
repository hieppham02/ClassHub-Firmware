# Dữ liệu thử cho phân tích vận hành ClassHub

Đã bổ sung dữ liệu nguồn `MOCK` vào `classhub_db` trên MariaDB cục bộ. Tên bảng
theo nghiệp vụ, không chứa chữ simulation. Các giá trị dòng điện, sự kiện cửa
và sự cố do chương trình sinh, **không phải phép đo thực tế**. Lịch sử phiên
được đọc từ DB để làm ngữ cảnh; chưa xác minh lịch sử này được tạo bằng cách nào.

## Phạm vi

| Mục | Giá trị |
| --- | --- |
| Mã đợt | `89cecf91-dd4d-5e19-903d-fe085a9b0848` |
| Thời gian | 24–30/09/2026, giờ Việt Nam; mốc kết thúc loại trừ: 01/10 00:00 |
| Phòng | DTD201, EAUT101, EAUT105 |
| Phiên làm ngữ cảnh | 33 phiên hoàn tất, thời gian hợp lệ, không chồng nhau trong phòng |
| Chu kỳ dự kiến | 30 giây |
| Mẫu dự kiến / thực ghi | 60.480 / 60.390 |
| Mẫu cố ý không gửi | 90 lượt đo thuộc 3 đợt mất bản tin |
| Sự kiện | 270 |
| Bối cảnh | 36: 33 phiên, 3 khoảng vệ sinh được phép |
| Đợt bất thường | 9: cửa mở lâu, dòng điện còn cao sau trả, mất bản tin; mỗi loại 3 đợt |
| Cửa sổ 5 phút | 6.048, trong đó 47 cửa sổ giao với ít nhất một đợt bất thường |

Thời điểm cảm biến và bảng mới lưu UTC. `received_at` / `returned_at` của phiên
nguồn được hiểu theo UTC+07:00, phù hợp cách backend đang đối chiếu phiên.
Khoảng `[start, end)` dùng nhất quán khi ghép dữ liệu. Cột `clock_synced=1`
chỉ là thuộc tính của bản tin sinh ra, không phải bằng chứng đồng hồ thiết bị thật.

## Lưu trữ trong DB

- `iot_sensor`: thêm các mẫu dòng điện, JSON tương thích cấu trúc hiện có
  (`sensorKey`, `metric=current`, `value`, `unit=A`, `quality=GOOD`).
- `data_batch`: seed, phiên bản bộ sinh, tham số, hồ sơ dòng điện giả định,
  danh sách node/boot ID và bản chốt các phiên nguồn tối giản.
- `data_batch_sensor`: liên kết mẫu cảm biến với đợt nhập; không cần sửa cấu trúc
  `iot_sensor` đang được backend sử dụng.
- `cabinet_event`: chuỗi yêu cầu mở → phát xung khóa → cửa mở → cửa đóng,
  cộng các sự kiện mất/kết nối lại node cảm biến.
- `operation_context`: khoảng sử dụng theo phiên và vệ sinh được phép.
- `anomaly_event`: nhãn chuẩn của bộ sinh, có `label_source=GENERATOR`.

Không phát MQTT, không kích khóa, không cập nhật trạng thái node/tủ hoặc ghi lại
phiên mượn. Không sao chép tên, mã sinh viên hoặc thông tin đăng nhập vào bộ dữ liệu.
Mẫu mới có thời gian trước mẫu cũ mới nhất, nên không thay đổi mẫu mới nhất mà
dashboard đang lấy. Backend chưa có API/UI riêng cho các bảng lịch sử mới.

## File đầu ra

Thư mục làm việc: `.codex-temp/ai-data/pilot/`.

- `power_samples.csv`, `cabinet_events.csv`, `operation_context.csv`: dữ liệu gốc.
- `source_sessions.csv`: bản chốt tối giản phiên nguồn, không có danh tính người mượn.
- `features_5min.csv`: các đặc trưng theo cửa sổ, **không có nhãn hoặc hồ sơ bộ sinh**.
- `labels_5min.csv`: nhãn ghép bằng cặp `(room_id, window_start_utc)`.
- `anomaly_labels.csv`: nhãn theo đợt, dùng để đánh giá khả năng phát hiện sự cố.
- `overview.png`, `episodes.png`: biểu đồ đã kiểm tra trực quan.
- `summary.json`, `validation.json`, `database-result.json`, `operations-check.json`:
  kết quả kiểm tra và số lượng thực tế.
- `insert.sql`, `rollback.sql`: SQL nhập/gỡ đúng đợt, không có mật khẩu.
- `dataset.json`: dữ liệu đầy đủ và tham số.

Bản ZIP bàn giao nằm trong `artifacts/`, chứa đầu ra và mã nguồn để không phụ
thuộc vào việc giữ thư mục tạm.

## Chạy lại

Từ thư mục dự án OV7670:

```powershell
py tools/ai_data/prepare.py
py tools/ai_data/verify.py
py tools/ai_data/apply.py --apply
```

`prepare.py` và `verify.py` chỉ đọc DB / ghi file. `apply.py --apply` mới ghi DB.
Nếu đợt đã có, chương trình xác minh nội dung và không nhập trùng. Nếu các bảng
lịch sử tồn tại mà đợt hiện tại chưa có, chương trình dừng để kiểm tra schema;
không tự xóa bảng hoặc thay thế dữ liệu khác. Định danh đợt phụ thuộc phiên bản,
seed và hash bản chốt phiên nguồn; nếu phiên nguồn thay đổi thì cần xem lại đợt mới.

Kết nối lấy từ User Secrets của backend hoặc biến môi trường
`ConnectionStrings__DefaultConnection`. Không lưu credential trong mã nguồn,
file SQL hay file xuất; mật khẩu chỉ được truyền qua môi trường tiến trình MySQL.
Máy hiện có MySQL CLI tại `E:/localhost/xampp/mysql/bin/mysql.exe`.

DDL chỉ thêm bảng, MariaDB commit DDL riêng. Dữ liệu đợt được nhập trong một
transaction; lỗi SQL làm transaction đó rollback khi CLI thoát. Bảng rỗng mới
tạo vẫn có thể còn nếu phần nhập dữ liệu thất bại.

`rollback.sql` chỉ gỡ mẫu `MOCK` có đúng batch membership và boot ID của đợt,
cùng sự kiện/bối cảnh/nhãn liên quan; giữ các bảng và dữ liệu cũ. Chỉ chạy khi
thực sự muốn gỡ đợt. `check_operations.py` thử gỡ trong một transaction rồi
**ROLLBACK**, do đó dữ liệu vẫn tồn tại sau kiểm tra.

## Giới hạn và bước tiếp

Đây là bộ thử pipeline, chưa phải bộ benchmark đủ để công bố độ chính xác:

- Các mức dòng điện được giả định, chưa hiệu chuẩn với phòng/thiết bị thật;
  không suy ra công suất W hay điện năng kWh từ các giá trị này.
- Chỉ có ba hồ sơ phòng và chín đợt bất thường; các đợt chưa trải đều theo ngày.
  Trước khi đánh giá theo thời gian, cần thêm nhiều cấu hình, ngày và độ khó,
  bảo đảm tập kiểm tra có các loại sự cố cần đánh giá.
- Ngoài phiên được ghi nhận không đồng nghĩa chắc chắn phòng không có người.
  Việc sử dụng ngoài lịch cần bối cảnh và kiểm tra thực tế.
- Các lần mất bản tin chỉ mô phỏng node cảm biến, không suy ra mất mạng toàn tủ.
- Khi học mô hình, bỏ các cột định danh khỏi đầu vào số, fit bước tiền xử lý trên
  tập train, ghép nhãn chỉ để đánh giá. Không dùng nhãn, mã đợt, seed, tham số
  hồ sơ hoặc chi tiết sự cố làm đặc trưng. Không xáo trộn các cửa sổ liền kề rồi
  coi chúng như các thử nghiệm độc lập.
- Chưa huấn luyện mô hình hoặc đưa AI vào quyết định mở khóa/hoàn tất trả.

Bước tiếp theo: mở rộng bộ sinh để tách train/validation/test theo thời gian
và phòng, bổ sung sự cố đa dạng, rồi so sánh ngưỡng nghiệp vụ với mô hình phát
hiện bất thường bằng số đợt phát hiện được, báo nhầm/phòng/ngày và độ trễ phát hiện.
