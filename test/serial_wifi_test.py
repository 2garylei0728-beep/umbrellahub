"""ESP32 v4.2 校園 WiFi（WPA3-H2E）連線驗證：抓取開機連線過程 → 判定是否連上 FCT_GUEST

用法：燒錄完成後立即執行（燒錄動作會重啟板子，開機即開始連線）：
    python serial_wifi_test.py [PORT]

PASS 條件（任一）：
    - 序列埠出現「成功！IP：」（connectWiFi 回傳 true）
    - 或出現「[WiFi] 已連上 AP」（WiFi 事件回報 STA_CONNECTED）
同時記錄：認證模式變更事件（6 = WPA3-SAE）、斷線原因碼（202 = 認證失敗）
"""
import serial, time, sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM11"
BAUD = 115200
WATCH_SECONDS = 100        # connectWiFi 最壞情況 4 輪約 60-70 秒 + 開機時間

ser = serial.Serial(PORT, BAUD, timeout=1)
time.sleep(0.5)
ser.reset_input_buffer()

lines = []
t0 = time.time()
connected = False
auth_change = []
reasons = []
print(f"=== 監聽 {PORT}（最長 {WATCH_SECONDS} 秒，等待開機 + WiFi 連線）===")
while time.time() - t0 < WATCH_SECONDS:
    line = ser.readline().decode(errors="ignore").strip()
    if not line:
        # 已連上且 10 秒無新輸出即提早結束
        if connected and time.time() - t0 > 15:
            time.sleep(2)
            while ser.in_waiting:
                extra = ser.readline().decode(errors="ignore").strip()
                if extra:
                    lines.append(extra)
                    print("[SER]", extra)
            break
        continue
    lines.append(line)
    print("[SER]", line)
    if "成功！IP：" in line or "已連上 AP" in line:
        connected = True
        print(">>> 已連線！繼續捕捉 8 秒後續事件 ...")
        t_end = time.time() + 8
        while time.time() < t_end:
            extra = ser.readline().decode(errors="ignore").strip()
            if extra:
                lines.append(extra)
                print("[SER]", extra)
                if "認證模式變更" in extra:
                    auth_change.append(extra)
                if "原因碼" in extra:
                    reasons.append(extra)
        break
    if "認證模式變更" in line:
        auth_change.append(line)
    if "原因碼" in line:
        reasons.append(line)

# 補充查詢 status / scan
print("\n=== 發送 status ===")
ser.write(b"status\n")
time.sleep(2)
while ser.in_waiting:
    extra = ser.readline().decode(errors="ignore").strip()
    if extra:
        lines.append(extra)
        print("[STATUS]", extra)

print("\n=== 發送 scan（列出周邊 AP 認證方式）===")
ser.write(b"scan\n")
time.sleep(6)
while ser.in_waiting:
    extra = ser.readline().decode(errors="ignore").strip()
    if extra:
        lines.append(extra)
        print("[SCAN]", extra)

ser.close()

wpa3 = any("→ 6" in a or "WPA3-SAE" in a for a in auth_change)
auth_fail = any("202" in r for r in reasons)

print("\n===== 驗證結果 =====")
print("WiFi 連線:", "PASS ✅" if connected else "FAIL ❌")
print("WPA3-SAE 協商事件:", "有（認證模式變更為 6）" if wpa3 else "未見（可能為 WPA2 路徑或未記錄）")
print("斷線原因碼紀錄:", reasons if reasons else "無")
if not connected and auth_fail:
    print("⚠️ 出現 202（AUTH_FAIL）——認證仍失敗，需進一步排查")
sys.exit(0 if connected else 1)
