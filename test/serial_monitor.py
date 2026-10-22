"""ESP32 即時串口監控（對話窗口專用）：持續讀取 COM11，
   把 ESP32 實際收到的網站指令與回覆即時記錄到日誌檔。
   日誌格式：[HH:MM:SS] 原始行
   用法：python serial_monitor.py [PORT] [持續秒數，預設 7200]"""
import serial, time, sys, os

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM11"
DURATION = int(sys.argv[2]) if len(sys.argv) > 2 else 7200
LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "serial_monitor.log")

ser = serial.Serial(PORT, 115200, timeout=1)
time.sleep(0.5)
ser.reset_input_buffer()

t0 = time.time()
count = 0
with open(LOG, "w", encoding="utf-8") as f:
    f.write(f"=== 監控開始 {time.strftime('%Y-%m-%d %H:%M:%S')} PORT={PORT} ===\n")
    f.flush()
    print(f"=== 即時監控 {PORT}（最長 {DURATION} 秒，日誌：{LOG}）===")
    while time.time() - t0 < DURATION:
        try:
            line = ser.readline().decode(errors="ignore").rstrip()
        except serial.SerialException as e:
            print("串口錯誤:", e)
            break
        if not line:
            continue
        ts = time.strftime("%H:%M:%S")
        count += 1
        # 標記關鍵行
        mark = ""
        if "\u25c4" in line or "收到指令" in line:
            mark = "  <<< 網站指令送達 ESP32"
        elif "\u25ba" in line or "回覆" in line:
            mark = "  <<< ESP32 執行完畢並回覆"
        elif "斷開" in line:
            mark = "  <<< 連線中斷"
        out = f"[{ts}] {line}{mark}"
        print(out)
        with open(LOG, "a", encoding="utf-8") as fa:
            fa.write(out + "\n")

print(f"=== 監控結束，共 {count} 行 ===")
ser.close()
