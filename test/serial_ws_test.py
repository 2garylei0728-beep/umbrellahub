"""ESP32 中繼伺服器（WebSocket）連線驗證：開機後等待 WS 握手成功

用法：燒錄完成後立即執行（燒錄會重啟板子）：
    python serial_ws_test.py [PORT]

PASS 條件：序列埠出現「[WS] 已連接中繼伺服器，在線控制就緒」
注意：Manus Autoscale 冷啟動可能需 30-90 秒，ESP32 每 5 秒重試，故觀察窗放到 180 秒。
"""
import serial, time, sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM11"
BAUD = 115200
WATCH_SECONDS = 180

ser = serial.Serial(PORT, BAUD, timeout=1)
time.sleep(0.5)
ser.reset_input_buffer()

lines = []
ws_lines = []
connected = False
t0 = time.time()
print(f"=== 監聽 {PORT}（最長 {WATCH_SECONDS} 秒，等待 WiFi + WS 連線）===")
while time.time() - t0 < WATCH_SECONDS:
    line = ser.readline().decode(errors="ignore").strip()
    if not line:
        continue
    lines.append(line)
    print("[SER]", line)
    if "[WS]" in line:
        ws_lines.append(line)
    if "已連接中繼伺服器" in line:
        connected = True
        print(">>> 中繼伺服器連線就緒！")
        break

# 查 status 確認中繼在線
print("\n=== 發送 status ===")
ser.write(b"status\n")
time.sleep(2)
while ser.in_waiting:
    extra = ser.readline().decode(errors="ignore").strip()
    if extra:
        lines.append(extra)
        print("[STATUS]", extra)

ser.close()

online_in_status = any("中繼伺服器在線" in l for l in lines)
print("\n===== 驗證結果 =====")
print("WS 連線事件:", "PASS ✅" if connected else "FAIL ❌")
print("status 顯示中繼在線:", "PASS ✅" if online_in_status else "（未確認）")
print("WS 相關日誌:")
for l in ws_lines:
    print("  ", l)
if not connected:
    print("⚠️ 未連上——若日誌只有重試無 4001，可能是伺服器冷啟動中，稍等再跑一次；")
    print("   若出現『握手失敗』或伺服器端拒絕，檢查 DEVICE_SECRET 是否與 Manus 設定一致。")
sys.exit(0 if connected else 1)
