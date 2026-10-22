"""ESP32 即時串口監控（桌面一鍵版）
   用法：esp32_monitor.py [COM埠，預設 COM11]
   用手機在網站上下指令，這裡即時顯示 ESP32 收到的命令與回執。
   ◄ 收到指令 = 網站指令送達 ESP32
   ► 回覆     = ESP32 執行完畢（有這個就是正常工作）
   停止：Ctrl+C 或關閉視窗"""
import serial, sys, time

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM11"

try:
    ser = serial.Serial(PORT, 115200, timeout=1)
except serial.SerialException as e:
    print("無法開啟 " + PORT + "：" + str(e))
    print("請確認：① ESP32 已用 USB 接上電腦 ② 沒有其他程式（Arduino IDE／其他監控）佔用該埠")
    sys.exit(1)

time.sleep(0.5)
ser.reset_input_buffer()
print("============================================")
print("  ESP32 即時指令監控  埠：" + PORT + " @115200")
print("  用手機下指令，這裡會即時顯示；Ctrl+C 停止")
print("============================================")

try:
    while True:
        line = ser.readline().decode(errors="ignore").rstrip()
        if not line:
            continue
        ts = time.strftime("%H:%M:%S")
        if "\u25c4" in line or "收到指令" in line:
            print("[" + ts + "] " + line + "    <<< 網站指令送達 ESP32")
        elif "\u25ba" in line or "回覆" in line:
            print("[" + ts + "] " + line + "    <<< ESP32 執行完畢")
        elif "斷開" in line:
            print("[" + ts + "] " + line + "    <<< 連線中斷")
        else:
            print("[" + ts + "] " + line)
except KeyboardInterrupt:
    print("=== 監控結束 ===")
finally:
    ser.close()
