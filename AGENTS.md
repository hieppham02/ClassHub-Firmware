# Quy tắc dùng ChatGPT Web và Codex tiết kiệm usage

## Mục tiêu
Hoàn thành đúng yêu cầu với ít thao tác và lượt đọc/chạy không cần thiết. Làm rõ phạm vi, giữ nguyên phần ngoài phạm vi và báo kết quả ngắn gọn.

## Quy tắc chung
- Xác định mục tiêu, đầu vào, phạm vi, giới hạn và dạng đầu ra.
- Nếu yêu cầu đủ rõ thì làm luôn; chỉ hỏi khi thiếu thông tin có thể ảnh hưởng đáng kể đến kết quả.
- Việc đơn giản thì xử lý thẳng; việc nhiều bước thì lập kế hoạch ngắn.
- Chỉ xem tài liệu, file, trang hoặc dữ liệu cần thiết. Với project lớn, khảo sát cấu trúc và entry point trước, rồi lần theo luồng liên quan.
- Không lặp thao tác đã hoàn tất nếu đầu vào chưa thay đổi.
- Chia việc lớn thành các phần có đầu ra kiểm tra được.
- Khi sửa, giữ nguyên API, nội dung và hành vi ngoài phạm vi được yêu cầu.
- Không bịa dữ liệu; nêu rõ giả định và phần chưa xác minh.
- Trả lời ngắn gọn, tập trung vào kết quả, thay đổi, cách kiểm tra và giới hạn đáng kể.

## Prompt cho ChatGPT Web
- Nêu rõ muốn giải thích, tra cứu, tóm tắt, soạn thảo hay chỉnh sửa.
- Đính kèm đúng file và nói rõ vai trò của từng file; chỉ rõ trang, mục, sheet hoặc đoạn cần làm.
- Yêu cầu định dạng đầu ra, độ dài, ngôn ngữ, người đọc và nguồn cần bám theo.
- Với thông tin có thể thay đổi hoặc cần độ chính xác, yêu cầu kiểm chứng và dẫn nguồn.
- Với tài liệu dài, yêu cầu xử lý theo từng phần và giữ nguyên phần không được chỉ định.

Mẫu:
> Mục tiêu: …  
> Tài liệu đầu vào: … (vai trò từng file)  
> Phạm vi: …  
> Giới hạn: …  
> Đầu ra: …  
> Kiểm chứng/nguồn: …

## Prompt cho Codex
- Nêu repo/thư mục, mục tiêu, module liên quan nếu biết, điều kiện không được thay đổi và tiêu chí hoàn thành.
- Ghi rõ chỉ phân tích, lập kế hoạch hay được phép sửa.
- Khi sửa, yêu cầu sửa tối thiểu, kiểm tra phần liên quan và tóm tắt file đã đổi cùng kết quả kiểm tra.
- Không cài package, sửa cấu hình, chạy lệnh dài/toàn bộ test suite hay truy cập mạng nếu không cần. Nếu cần thao tác tốn thời gian hoặc thay đổi môi trường, nêu lý do trước.
- Không nói test thành công nếu chưa chạy; phân biệt test đã chạy và chưa chạy.
- Với yêu cầu đọc cả repo, lập bản đồ entry point, module, luồng dữ liệu và file quan trọng trước; chỉ đào sâu phần cần thiết.

Mẫu phân tích:
> Chỉ phân tích, chưa sửa. Khảo sát [thư mục/module] để tìm [vấn đề]. Chỉ đọc phần cần thiết. Nêu nguyên nhân, file liên quan, bằng chứng và điểm chưa chắc chắn.

Mẫu sửa:
> Sửa [lỗi/tính năng] trong [module]. Được phép sửa các file liên quan. Giữ nguyên [API/hành vi]. Tránh [điều cấm]. Chạy kiểm tra liên quan gần nhất. Báo file đã đổi, thay đổi chính và kết quả kiểm tra.

Mẫu khảo sát project:
> Chưa sửa code. Khảo sát cấu trúc project và lần theo luồng [tên luồng]. Xác định entry point, module tham gia và luồng dữ liệu. Không đọc sâu phần không liên quan. Trả về sơ đồ ngắn và file cần xem tiếp.

## Dùng Plan và Skill
- Dùng Plan cho việc nhiều bước, nhiều file/module, có phụ thuộc hoặc cần theo dõi tiến độ.
- Không dùng Plan cho câu hỏi nhanh, sửa nhỏ hoặc việc một bước.
- Chỉ dùng Skill khi tác vụ đúng lĩnh vực Skill hướng dẫn, như tài liệu, slide hoặc spreadsheet.
- Nêu rõ file tham chiếu, nội dung cần giữ, đầu ra và cách kiểm tra.
- Đọc đúng phần hướng dẫn cần thiết; không gọi nhiều Skill chỉ vì tên có vẻ liên quan.

## Dùng file hiệu quả
- Với code, mở đúng thư mục gốc để Codex thấy README, `AGENTS.md` và cấu trúc; chỉ định module mục tiêu.
- Với tài liệu, nói rõ file nào là đề bài/nguồn, file nào cần sửa, và cần sửa trực tiếp hay tạo bản mới.
- Với bảng tính, nêu sheet, vùng ô, cột hoặc phép tính cần xem.
- Với PDF, nêu trang/mục và phần cần giữ nguyên.
- Với dữ liệu lớn, chỉ cung cấp mẫu hoặc phạm vi cần thiết; tránh dán lặp nội dung đã có trong file.
- Đặt tên file rõ nghĩa và nói rõ bản nào mới nhất.

## Nguyên tắc tiết kiệm usage
1. Khoanh đúng câu hỏi và phạm vi.
2. Khảo sát trước, đọc sâu đúng nơi cần thiết.
3. Chia tác vụ lớn thành các chặng có đầu ra cụ thể.
4. Chạy lệnh/test nhỏ nhất đủ xác nhận thay đổi.
5. Tránh yêu cầu mơ hồ như “đọc hết rồi tối ưu tất cả”.