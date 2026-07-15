#!/usr/bin/env python3
"""
ESP-NOW 投屏工具

自动将图片压缩为 240×240 RGB565 格式，通过串口发送给基站转发到接收端。
支持 PNG / JPG / BMP 等常见格式。

用法:
  # 自动读取接收端 MAC + 传图（无图片则基站自生成）
  python3 tools/send.py /dev/cu.usbserial-1140                          # 使用已保存的 MAC
  python3 tools/send.py /dev/cu.usbserial-1140 8C:4F:00:53:A3:18        # 指定 MAC
  python3 tools/send.py /dev/cu.usbserial-1130 /dev/cu.usbserial-1140   # 自动读取接收端 MAC

  # 发送图片文件（自动缩放至 240×240）
  python3 tools/send.py /dev/cu.usbserial-1140 8C:4F:00:53:A3:18 photo.jpg
  python3 tools/send.py /dev/cu.usbserial-1140 8C:4F:00:53:A3:18 ~/Pictures/test.png
"""

import sys
import time
import re
import serial
import os
import struct
from PIL import Image


# === 协议常量 ===
CMD_IMG_START  = 0x01
CMD_STRIP_DATA = 0x02
CMD_IMG_END    = 0x03

IMG_WIDTH  = 240
IMG_HEIGHT = 240
STRIP_H    = 8
STRIP_BYTES = IMG_WIDTH * STRIP_H * 2  # 3840

MAC_FILE = os.path.join(os.path.dirname(__file__), 'receiver_mac.txt')


# ======================================================================
# 串口操作
# ======================================================================

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
    """配置基站 MAC 地址"""
    ser = serial.Serial(port, 115200, timeout=2)
    ser.reset_input_buffer()

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
        return None

    ser.write(f'{mac}\n'.encode())
    ser.flush()
    time.sleep(2)

    resp = ser.read(4096).decode('utf-8', errors='replace')
    ok = 'Using MAC' in resp
    return ser if ok else None


def send_serial_packet(ser, cmd, payload=b''):
    """发送串口协议包"""
    pkt = bytes([cmd]) + struct.pack('<H', len(payload)) + payload
    ser.write(pkt)
    ser.flush()


def load_image_strips(path):
    """加载图片并转换为 RGB565 逐行数据"""
    img = Image.open(path).convert('RGB')
    img = img.resize((IMG_WIDTH, IMG_HEIGHT), Image.LANCZOS)
    pixels = img.load()

    strips = []
    for strip_idx in range(IMG_HEIGHT // STRIP_H):
        data = bytearray()
        for py in range(STRIP_H):
            y = strip_idx * STRIP_H + py
            for x in range(IMG_WIDTH):
                r, g, b = pixels[x, y]
                c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3)
                data += bytes([c & 0xFF, (c >> 8) & 0xFF])
        strips.append(bytes(data))
    return strips


def send_image_via_serial(ser, strips, verbose=True):
    """通过串口发送完整图片"""
    if verbose:
        print(f'Sending image ({len(strips)} strips)...')

    send_serial_packet(ser, CMD_IMG_START)
    if verbose:
        print(f'  START')
    time.sleep(0.02)

    for idx, strip_data in enumerate(strips):
        payload = bytes([idx]) + strip_data
        send_serial_packet(ser, CMD_STRIP_DATA, payload)
        if verbose and (idx % 5 == 0 or idx == len(strips) - 1):
            print(f'  Strip {idx}/{len(strips)-1}')
        # 等待基站处理（可环境变量 SERIAL_STRIP_DELAY 覆盖，单位秒）
        time.sleep(float(os.environ.get('SERIAL_STRIP_DELAY', '0.01')))

    send_serial_packet(ser, CMD_IMG_END)
    if verbose:
        print(f'  END')


def monitor_receiver(port):
    """实时监视接收端传输统计"""
    ser = serial.Serial(port, 115200, timeout=1)
    ser.reset_input_buffer()
    print(f'\nMonitoring receiver... (Ctrl+C to stop)')

    try:
        while True:
            line = ser.readline().decode('utf-8', errors='replace').strip()
            if not line:
                continue

            # 显示关键统计行
            if any(kw in line for kw in
                   ['Complete', 'Received', 'Lost', 'Speed']):
                print(f'  {line}')

    except KeyboardInterrupt:
        print('\nDone.')
    ser.close()


# ======================================================================
# 主逻辑
# ======================================================================

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    port = sys.argv[1]       # 基站串口

    # --- 获取接收端 MAC ---
    mac = None

    # 方式1: 从保存的文件读取
    if os.path.exists(MAC_FILE) and len(sys.argv) < 4:
        with open(MAC_FILE) as f:
            mac = f.read().strip()
        print(f'Using saved MAC: {mac}')

    # 方式2: 从命令行参数读取
    if len(sys.argv) >= 3:
        arg2 = sys.argv[2]
        if ':' in arg2:  # 是 MAC 格式
            mac = arg2.upper()
        else:  # 是接收端串口
            rx_port = arg2
            print(f'Reading MAC from receiver ({rx_port})...', end=' ', flush=True)
            mac = read_mac_from_receiver(rx_port)
            if not mac:
                print('FAILED')
                sys.exit(1)
            print(mac)
            # 保存
            with open(MAC_FILE, 'w') as f:
                f.write(mac)
            print(f'Saved to {MAC_FILE}')

    if not mac:
        print('Error: No receiver MAC. Specify MAC or receiver port.')
        print(__doc__)
        sys.exit(1)

    # --- 配置基站 ---
    ser = config_base(port, mac)
    if not ser:
        print('Failed to configure base station')
        sys.exit(1)
    print(f'Base station configured (port={port}, MAC={mac})')

    # --- 发送图片 ---
    image_file = None
    if len(sys.argv) >= 4:
        image_file = sys.argv[3]
    elif len(sys.argv) >= 3 and os.path.isfile(sys.argv[2]):
        image_file = sys.argv[2]

    if image_file:
        print(f'Loading image: {image_file}')
        try:
            strips = load_image_strips(image_file)
            send_image_via_serial(ser, strips)
        except Exception as e:
            print(f'Image error: {e}')
            print('Falling back to base station self-generating mode')
    else:
        print('Base station will self-generate test pattern')

    ser.close()
    time.sleep(1)

    # --- 监视 ---
    if len(sys.argv) >= 3 and ':' not in sys.argv[2] and not image_file:
        # 指定了接收端端口 → 监视它
        monitor_receiver(sys.argv[2])
    else:
        print('\nDone. Press Ctrl+C to exit.')


if __name__ == '__main__':
    main()
