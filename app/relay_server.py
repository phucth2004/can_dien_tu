#!/usr/bin/env python3
"""
relay_server.py - Relay server chạy trên máy tính,
                  nhận GET từ BBB và forward lên Google Sheets.

Luồng dữ liệu:
  alarm_core (BBB)
    → wget "http://192.168.4.2:8000/?timestamp=...&weight=...&status=..."
    → relay_server.py (máy tính này)
    → Google Apps Script doGet()
    → Google Sheets

Lỗi đã sửa so với bản gốc:
  [FIX 1] Log rõ query nhận được và query forward để dễ debug
  [FIX 2] Kiểm tra query rỗng và các field bắt buộc trước khi gửi
  [FIX 3] Thread daemon=True để server thoát sạch khi Ctrl+C
  [FIX 4] Tắt log mặc định của BaseHTTPRequestHandler (quá verbose)
  [FIX 5] Bind 0.0.0.0 thay vì IP cụ thể để linh hoạt hơn
"""

import threading
import logging
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs
import urllib.request
import urllib.error

# ------------------------------------------------------------------ #
WEBHOOK = (
    "https://script.google.com/macros/s/"
    "AKfycbwlGn1mneD7RdsSlQykDW290mSOwhpZ_awTCmyyU6eV30HvP0jiAAa7-yQnZvf10lM"
    "/exec"
)

LISTEN_HOST = "0.0.0.0"    # Nhận từ mọi interface (bao gồm 192.168.4.2)
LISTEN_PORT = 8000
TIMEOUT_S   = 10
# ------------------------------------------------------------------ #


def send_to_google(query: str):
    """Forward query string lên Google Apps Script qua GET."""
    full_url = f"{WEBHOOK}?{query}"
    try:
        with urllib.request.urlopen(full_url, timeout=TIMEOUT_S) as resp:
            body = resp.read().decode("utf-8", errors="replace")
            print(f"[RELAY] OK -> Google: {query} | Phan hoi: {body}")
    except urllib.error.HTTPError as e:
        print(f"[RELAY LOI] HTTP {e.code}: {e.reason} | query={query}")
    except urllib.error.URLError as e:
        print(f"[RELAY LOI] Mang: {e.reason} | query={query}")
    except Exception as e:
        print(f"[RELAY LOI] {e} | query={query}")


class RelayHandler(BaseHTTPRequestHandler):

    def do_GET(self):
        # [FIX 1] Parse và validate query trước
        parsed = urlparse(self.path)
        query  = parsed.query

        # [FIX 4] Trả lời BBB ngay lập tức (không để wget timeout)
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(b"OK")

        if not query:
            print(f"[RELAY] Nhan request khong co query: {self.path} — bo qua")
            return

        # [FIX 2] Kiểm tra đủ 3 field bắt buộc
        params = parse_qs(query)
        missing = [f for f in ("timestamp", "weight", "status")
                   if f not in params]
        if missing:
            print(f"[RELAY] Thieu field {missing} trong query: {query} — bo qua")
            return

        print(f"[RELAY] Nhan tu BBB: {query}")

        # [FIX 3] daemon=True: thread tự kết thúc khi main thread thoát
        t = threading.Thread(target=send_to_google, args=(query,), daemon=True)
        t.start()

    def log_message(self, fmt, *args):
        # [FIX 4] Tắt log mặc định quá verbose của BaseHTTPRequestHandler
        # Chỉ giữ log của chúng ta (print ở trên)
        pass


if __name__ == "__main__":
    server = HTTPServer((LISTEN_HOST, LISTEN_PORT), RelayHandler)
    print(f"[RELAY SERVER] Lang nghe tai http://{LISTEN_HOST}:{LISTEN_PORT}")
    print(f"[RELAY SERVER] Forward -> {WEBHOOK[:60]}...")
    print("[RELAY SERVER] Nhan Ctrl+C de dung\n")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[RELAY SERVER] Dung.")
        server.server_close()
