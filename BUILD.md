# Hướng dẫn build & chạy — FJSP+ SAT solver (C++ / CaDiCaL rel-1.9.5)

Đây là bản chuyển ngữ C++ của `bisec.py`, dùng **CaDiCaL** (bản `rel-1.9.5`, clone trực
tiếp từ repo của tác giả Armin Biere: <https://github.com/arminbiere/cadical>) làm SAT
backend, thay cho `pysat`/`Kissat404` trong bản Python.

> Lưu ý: mình không có mạng trong sandbox hiện tại nên **không tự clone/build/link được
> CaDiCaL thật** để test end-to-end. Mình đã kiểm tra kỹ code bằng cách biên dịch (syntax
> + type check) với một header giả lập đúng chữ ký API công khai của CaDiCaL
> (`CaDiCaL::Solver`, `CaDiCaL::Terminator`, `add/solve/val/vars/connect_terminator/set`)
> — biên dịch sạch, 0 lỗi/cảnh báo. Bạn cần build thật ở máy có mạng theo các bước dưới.

## 1. Cấu trúc project

```
main.cpp             - CLI, vòng lặp tìm kiếm nhị phân song song, in/ghi kết quả
instance.hpp          - đọc instance JSON, verify(), heuristic tham lam, lower_bound()
cnf_encoder.hpp       - Order Encoding (mục 4 trong Setup_Time_FJSSP.pdf)
cadical_worker.hpp    - wrapper gọi CaDiCaL::Solver + Terminator để hủy/timeout
json_min.hpp          - JSON parser tối giản tự viết (không phụ thuộc ngoài)
Makefile              - tự clone + build CaDiCaL rel-1.9.5 rồi build chương trình
```

## 2. Build

```bash
cd cpp
make            # sẽ tự "git clone --branch rel-1.9.5 https://github.com/arminbiere/cadical.git"
                # vào third_party/cadical, chạy ./configure && make trong đó,
                # rồi biên dịch fjsp_sat và link tĩnh với libcadical.a
```

Nếu máy không có sẵn `git`/trình biên dịch C, cài trước:

```bash
sudo apt-get update && sudo apt-get install -y git build-essential
```

Nếu muốn tự tay clone/build CaDiCaL (thay vì để Makefile làm hộ):

```bash
git clone --branch rel-1.9.5 https://github.com/arminbiere/cadical.git third_party/cadical
cd third_party/cadical
./configure && make          # sinh ra build/libcadical.a và src/cadical.hpp
cd ../..
g++ -std=c++17 -O2 -pthread -Ithird_party/cadical/src \
    main.cpp third_party/cadical/build/libcadical.a -o fjsp_sat -pthread
```

## 3. Chạy

```bash
./fjsp_sat <duong_dan_instance.json> \
    [--time-limit 600] [--output ket_qua.json] \
    [--ub N] [--seed 0] [--workers 4]
```

Ví dụ với file mẫu:

```bash
./fjsp_sat /duong/dan/sm_0_1.json --time-limit 300 --output out.json --workers 4
```

Tham số:
- `--time-limit` (giây): tổng ngân sách thời gian (heuristic + tìm kiếm SAT).
- `--output/-o`: ghi lịch tối ưu/khả thi ra file JSON.
- `--ub`: gợi ý upper bound ban đầu (tùy chọn, nếu không đưa sẽ dùng heuristic).
- `--seed`: seed cho heuristic ngẫu nhiên.
- `--workers`: số bound SAT giải song song cùng lúc (mỗi bound = 1 `CaDiCaL::Solver`
  chạy trong 1 process riêng qua `fork()`; kết quả được truyền về process chính qua pipe.

## 4. Khác biệt so với `bisec.py`

- **Song song hoá**: mỗi bound được chạy trong một process hệ điều hành riêng bằng
  `fork()`. Process con tạo `CaDiCaL::Solver`, gửi trạng thái/model về process cha qua
  pipe, rồi thoát. Khi một bound khác làm worker trở nên lỗi thời, process cha gửi
  `SIGTERM` và thu hồi process đó; timeout của CaDiCaL vẫn được kiểm soát bằng
  `TimeTerminator`. Về mặt logic tìm kiếm (chia khoảng theo tỉ lệ 3/4, huỷ các bound
  đã lỗi thời khi có SAT/UNSAT mới, dừng khi `lo > upper`) được giữ y hệt Python.
- Không có `--solver auto|pysat|builtin` vì bản C++ chỉ dùng CaDiCaL trực tiếp.
- Không in "Sequence on each machine" (bản Python cũng đã comment phần này).

## 5. Kiểm tra kết quả độc lập

`verify()` trong `instance.hpp` kiểm tra lại lịch đầu ra (release time, precedence,
setup/transport, không chồng lấp máy) hoàn toàn độc lập với phần mã hoá CNF — in ra
"HOP LE"/"LOI" sau mỗi lần chạy, giống hệt `verify()` trong `bisec.py`.
