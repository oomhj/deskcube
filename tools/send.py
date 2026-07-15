#!/usr/bin/env python3
"""
ESP-NOW 投屏发送/监控脚本

自动配置基站并实时显示传输统计。

用法:
  # 自动读取接收端 MAC + 配置基站 + 实时监控
  python3 tools/send.py /dev/cu.usbserial-1130 /dev/cu.usbserial-1140

  # 使用已保存的 MAC 快速启动
  python3 tools/send.py /dev/cu.usbserial-1140
"""

import sys
import time
import re
import serial
import os


def read_mac(port, timeout=5):
    """读取接收端 MAC"""
    ser = serial.Serial(port, 115200, timeout=2)
    ser.dtr = False
    time.sleep(0.3)
    ser.dtr = True
    ser.reset_input_buffer()

    deadline = time.time() + timeout
    while time.time() < deadline:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        m = re.search(
            r'([0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}:'
            r'[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2})', line
        )
        if m:
            ser.close()
            return m.group(1).upper()
        time.sleep(0.1)
    ser.close()
    return None


def config_base(port, mac, timeout=10):
    """配置基站 MAC"""
    ser = serial.Serial(port, 115200, timeout=2)
    ser.reset_input_buffer()

    # 等待 MAC 输入提示
    deadline = time.time() + timeout
    prompted = False
    while time.time() < deadline:
        data = ser.read(512).decode('utf-8', errors='replace')
        if 'Enter receiver MAC' in data or '>' in data:
            prompted = True
            break
        time.sleep(0.2)

    if not prompted:
        ser.close()
        return False

    # 发送 MAC
    ser.write(f'{mac}\n'.encode())
    ser.flush()
    time.sleep(2)

    # 读取确认
    resp = ser.read(4096).decode('utf-8', errors='replace')
    ok = 'Using MAC' in resp
    ser.close()
    return ok


def monitor(port):
    """监视接收端统计信息"""
    ser = serial.Serial(port, 115200, timeout=1)
    ser.reset_input_buffer()
    print(f'\n{"="*50}')
    print(f'Monitoring receiver on {port}')
    print('Press Ctrl+C to stop')
    print(f'{"="*50}')
    print(f'{"Frame":>6}  {"Received":>8}  {"Lost":>6}  {"Speed":>8}')
    print(f'{"-"*6}  {"-"*8}  {"-"*6}  {"-"*8}')

    frame = 0
    received = lost = 0
    speed = 0.0
    try:
        while True:
            line = ser.readline().decode('utf-8', errors='replace').strip()
            if not line:
                continue
            m = re.search(r'Received:\s*(\d+)/(\d+)', line)
            if m:
                received = int(m.group(1))
                continue
            m = re.search(r'Lost:\s*(\d+)', line)
            if m:
                lost = int(m.group(1))
                continue
            m = re.search(r'Speed:\s*([\d.]+)\s*KB/s', line)
            if m:
                speed = float(m.group(1))
                frame += 1
                print(f'{frame:>6}  {received:>8}  {lost:>6}  {speed:>7.1f} KB/s')
    except KeyboardInterrupt:
        print(f'\n{"="*50}')
        print(f'Stats: {frame} frames, last: {received}/{received+lost} lost={lost}')
    ser.close()


def main():
    # MAC 文件路径
    mac_file = os.path.join(os.path.dirname(__file__), 'receiver_mac.txt')

    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    if '--monitor' in sys.argv:
        port = sys.argv[1]
        monitor(port)
        return

    # 从文件读取 MAC
    saved_mac = None
    if os.path.exists(mac_file):
        with open(mac_file) as f:
            saved_mac = f.read().strip()

    # 指定了接收端端口 → 从接收端读取 MAC
    if len(sys.argv) >= 3 and not saved_mac:
        rx_port = sys.argv[1]
        tx_port = sys.argv[2]

        print(f'Reading MAC from receiver ({rx_port})...', end=' ', flush=True)
        mac = read_mac(rx_port)
        if not mac:
            print('FAILED')
            sys.exit(1)
        print(f'{mac}')
        # 保存 MAC
        with open(mac_file, 'w') as f:
            f.write(mac)
        print(f'Saved to {mac_file}')

    # 使用保存的 MAC 直接配置基站
    elif saved_mac and len(sys.argv) >= 2:
        mac = saved_mac
        tx_port = sys.argv[1]
        if len(sys.argv) >= 3:
            tx_port = sys.argv[2]
        print(f'Using saved MAC: {mac}')

    else:
        print(__doc__)
        sys.exit(1)

    print(f'Configuring base ({tx_port})...', end=' ', flush=True)
    if config_base(tx_port, mac):
        print('OK')
    else:
        print('FAILED')
        sys.exit(1)

    time.sleep(2)
    monitor(rx_port if not saved_mac else sys.argv[1])
        return

    print(__doc__)
    sys.exit(1)


if __name__ == '__main__':
    main()
