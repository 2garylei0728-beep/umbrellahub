"""ESP32 v4.1 LED 功能串口验证：抓取开机信息 → 跑 led test → 查 status"""
import serial, time, sys

PORT = "COM11"
BAUD = 115200

ser = serial.Serial(PORT, BAUD, timeout=1)
time.sleep(1.0)
ser.reset_input_buffer()

# 1) 等待并抓取开机信息（打开串口会重置板子）
boot = []
t0 = time.time()
while time.time() - t0 < 12:
    line = ser.readline().decode(errors="ignore").strip()
    if line:
        boot.append(line)
        print("[BOOT]", line)
    if any("操作指示燈" in l for l in boot):
        time.sleep(1.5)
        break

# 2) 运行 led test（约 7 秒）
print("\n=== 发送 led test ===")
ser.write(b"led test\n")
time.sleep(8.5)
while ser.in_waiting:
    line = ser.readline().decode(errors="ignore").strip()
    if line:
        print("[TEST]", line)

# 3) 查询 status（应显示指示灯：待机）
print("\n=== 发送 status ===")
ser.write(b"status\n")
time.sleep(2)
while ser.in_waiting:
    line = ser.readline().decode(errors="ignore").strip()
    if line:
        print("[STATUS]", line)

ser.close()

ok_boot = any("啟動（在線控制版 v4.1）" in l for l in boot)
ok_led_pin = any("操作指示燈：GPIO" in l for l in boot)
print("\n===== 验证结果 =====")
print("v4.1 开机标记:", "PASS" if ok_boot else "FAIL")
print("指示灯引脚标记:", "PASS" if ok_led_pin else "FAIL")
sys.exit(0 if (ok_boot and ok_led_pin) else 1)
