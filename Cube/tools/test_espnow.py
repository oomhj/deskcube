#!/usr/bin/env python3
"""
ESP-NOW 投屏测试脚本

自动完成：
  1. 从接收端读取 MAC 地址（自动复位）
  2. 将 MAC 发送给基站
  3. 显示传输统计

用法:
  # 两端自动配置
  python3 test_espnow.py /dev/cu.usbserial-1130 /dev/cu.usbserial-1140

  # 手动指定 MAC
  python3 test_espnow.py /dev/cu.usbserial-1140 --mac 8C:4F:00:53:A3:18
"""

import sys
import time
import re
import serial


def read_mac_from_receiver(port, timeout=5):
    """复位接收端，读取 MAC 地址"""
    ser = serial.Serial(port, 115200, timeout=2)
    ser.dtr = False
    time.sleep(0.3)
    ser.dtr = True
    ser.reset_input_buffer()

    deadline = time.time() + timeout
    while time.time() < deadline:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        m = re.search(
            r'([0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2})',
            line
        )
        if m:
            ser.close()
            return m.group(1).upper()
        time.sleep(0.1)

    ser.close()
    return None


def send_mac_to_base(port, mac, timeout=10):
    """连接基站，等待 MAC 提示后发送 MAC"""
    ser = serial.Serial(port, 115200, timeout=3)
    ser.reset_input_buffer()

    deadline = time.time() + timeout
    while time.time() < deadline:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        if 'Enter receiver MAC' in line or line == '>':
            break
        time.sleep(0.1)

    ser.write(f'{mac}\n'.encode())
    ser.flush()
    time.sleep(1)

    resp = ser.read(1024).decode('utf-8', errors='replace')
    for line in resp.split('\n'):
        l = line.strip()
        if l:
            print(f'  {l}')

    return ser


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    if '--mac' in sys.argv:
        idx = sys.argv.index('--mac')
        mac = sys.argv[idx + 1].upper()
        port = sys.argv[1]
        print(f'Sending MAC {mac} to base on {port}...')
        send_mac_to_base(port, mac)
        print('Done')
        return

    if len(sys.argv) >= 3:
        rx_port = sys.argv[1]
        tx_port = sys.argv[2]

        print(f'Reading MAC from receiver ({rx_port})...')
        mac = read_mac_from_receiver(rx_port)
        if not mac:
            print('✗ Failed - check receiver connection')
            sys.exit(1)
        print(f'  Receiver MAC: {mac}')

        print(f'Configuring base ({tx_port})...')
        ser = send_mac_to_base(tx_port, mac)
        ser.close()

        print(f'\n✓ Done! Receiver={rx_port}  Base={tx_port}  MAC={mac}')
        return

    print(__doc__)
    sys.exit(1)


if __name__ == '__main__':
    main()
