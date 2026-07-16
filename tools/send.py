#!/usr/bin/env python3
"""
ESP-NOW 投屏工具（JPEG 模式）

用法:
  # 直传 JPEG（文件必须是 240×240）
  python3 tools/send.py /dev/cu.usbserial-1140 8C:4F:00:53:A3:18 --raw-jpeg image.jpg

  # PIL 缩放 + JPEG 编码
  python3 tools/send.py /dev/cu.usbserial-1140 8C:4F:00:53:A3:18 photo.png
  python3 tools/send.py /dev/cu.usbserial-1140 8C:4F:00:53:A3:18 --jpeg photo.png
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
CMD_IMG_END    = 0x03
CMD_JPG_START  = 0x10
CMD_JPG_DATA   = 0x11
CMD_CMD        = 0x20

IMG_WIDTH   = 240
IMG_HEIGHT  = 240
TOTAL_STRIPS = 30

MAC_FILE = os.path.join(os.path.dirname(__file__), 'receiver_mac.txt')
STRIP_ACK_TIMEOUT = float(os.environ.get('STRIP_ACK_TIMEOUT', '30.0'))


# ======================================================================
# 串口操作
# ======================================================================

def read_mac_from_receiver(port, timeout=5):
    """复位接收端，读取 MAC 地址"""
    ser = serial.Serial(port, 115200, timeout=2)
    ser.dtr = False; time.sleep(0.3)
    ser.dtr = True; ser.reset_input_buffer()
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        m = re.search(r'([0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}:'
                       r'[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2})', line)
        if m: ser.close(); return m.group(1).upper()
        time.sleep(0.1)
    ser.close(); return None


def config_base(port, mac, timeout=10):
    """配置基站 MAC 地址"""
    ser = serial.Serial(port, 115200, timeout=2)
    ser.reset_input_buffer()
    deadline = time.time() + timeout
    prompted = False
    while time.time() < deadline:
        data = ser.read(512).decode('utf-8', errors='replace')
        if 'Enter receiver MAC' in data or '>' in data: prompted = True; break
        time.sleep(0.2)
    if not prompted: ser.close(); return None
    ser.write(f'{mac}\n'.encode()); ser.flush(); time.sleep(2)
    resp = ser.read(4096).decode('utf-8', errors='replace')
    return ser if 'Using MAC' in resp else None


def send_serial_packet(ser, cmd, payload=b''):
    """发送串口协议包（带 XOR 校验）"""
    pkt = bytes([cmd]) + struct.pack('<H', len(payload)) + payload
    xor = cmd ^ (len(payload) & 0xFF) ^ ((len(payload) >> 8) & 0xFF)
    for b in payload: xor ^= b
    ser.write(pkt + bytes([xor & 0xFF]))
    ser.flush()


def wait_strip_ack(ser):
    """等待基站 ACK (0x06)"""
    deadline = time.time() + STRIP_ACK_TIMEOUT
    while time.time() < deadline:
        b = ser.read(1)
        if b == b'\x06': return True
        if b == b'': time.sleep(0.001)
    return False


# ======================================================================
# JPEG 发送模式
# ======================================================================

def send_raw_jpeg_via_serial(ser, jpg_path, verbose=True, chunk_size=512):
    """直传 JPEG 文件（必须是 240×240，不做任何处理）"""
    with open(jpg_path, 'rb') as f:
        jpg_data = f.read()

    if verbose:
        print(f'Raw JPEG: {jpg_path} ({len(jpg_data)} bytes, '
              f'{len(jpg_data)//chunk_size+1} chunks)')

    send_serial_packet(ser, CMD_JPG_START, struct.pack('<H', len(jpg_data)))
    if verbose: print(f'  JPG START ({len(jpg_data)} bytes)')

    for offset in range(0, len(jpg_data), chunk_size):
        chunk = jpg_data[offset:offset + chunk_size]
        send_serial_packet(ser, CMD_JPG_DATA, chunk)
    if verbose: print(f'  JPG DATA sent')

    total = 0
    while total < TOTAL_STRIPS:
        if wait_strip_ack(ser):
            total += 1
            if verbose and (total % 5 == 0 or total == TOTAL_STRIPS):
                print(f'  Strip {total}/{TOTAL_STRIPS} ✓')
        else:
            print(f'\n  ERROR: ACK timeout on strip {total}')
            return False

    send_serial_packet(ser, CMD_IMG_END)
    if verbose: print(f'  END')
    return True


def send_jpeg_via_serial(ser, jpg_path, verbose=True, chunk_size=512, quality=70):
    """PIL 缩放 → JPEG 编码 → 串口发送"""
    img = Image.open(jpg_path).convert('RGB')
    img = img.resize((IMG_WIDTH, IMG_HEIGHT), Image.LANCZOS)
    import io
    buf = io.BytesIO()
    img.save(buf, format='JPEG', quality=quality)
    jpg_data = buf.getvalue()

    if verbose:
        print(f'JPEG: {jpg_path} → {IMG_WIDTH}×{IMG_HEIGHT} ({len(jpg_data)} bytes,'
              f' Q={quality}, {len(jpg_data)//chunk_size+1} chunks)')

    send_serial_packet(ser, CMD_JPG_START, struct.pack('<H', len(jpg_data)))
    if verbose: print(f'  JPG START ({len(jpg_data)} bytes)')

    for offset in range(0, len(jpg_data), chunk_size):
        chunk = jpg_data[offset:offset + chunk_size]
        send_serial_packet(ser, CMD_JPG_DATA, chunk)
    if verbose: print(f'  JPG DATA sent')

    total = 0
    while total < TOTAL_STRIPS:
        if wait_strip_ack(ser):
            total += 1
            if verbose and (total % 5 == 0 or total == TOTAL_STRIPS):
                print(f'  Strip {total}/{TOTAL_STRIPS} ✓')
        else:
            print(f'\n  ERROR: ACK timeout on strip {total}')
            return False

    send_serial_packet(ser, CMD_IMG_END)
    if verbose: print(f'  END')
    return True


# ======================================================================
# 指令发送
# ======================================================================

def send_brightness(ser, value, verbose=True):
    """发送亮度调节指令"""
    value = max(0, min(100, int(value)))
    # 指令包: [cmd_id(1)][cmd_len(1)][params...]
    payload = bytes([0x01, 0x01, value])  # cmd=SET_BRIGHTNESS, len=1, value
    send_serial_packet(ser, CMD_CMD, payload)
    if verbose: print(f'Brightness set to {value}')
    return True


# ======================================================================
# 主逻辑
# ======================================================================

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)

    port = sys.argv[1]

    # --- 获取 MAC ---
    mac = None
    if os.path.exists(MAC_FILE) and len(sys.argv) < 4:
        with open(MAC_FILE) as f: mac = f.read().strip()
        print(f'Using saved MAC: {mac}')

    if len(sys.argv) >= 3:
        arg2 = sys.argv[2]
        if ':' in arg2:
            mac = arg2.upper()
        elif os.path.isfile(arg2):
            pass
        else:
            rx_port = arg2
            print(f'Reading MAC from receiver ({rx_port})...', end=' ', flush=True)
            mac = read_mac_from_receiver(rx_port)
            if not mac: print('FAILED'); sys.exit(1)
            print(mac)
            with open(MAC_FILE, 'w') as f: f.write(mac)
            print(f'Saved to {MAC_FILE}')

    if not mac:
        print('Error: No receiver MAC.'); print(__doc__); sys.exit(1)

    # --- 配置基站 ---
    ser = config_base(port, mac)
    if not ser: print('Failed to configure base station'); sys.exit(1)
    print(f'Base station configured (port={port}, MAC={mac})')

    # --- 解析参数 ---
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    flags = [a for a in sys.argv[1:] if a.startswith('--')]
    use_raw_jpeg = '--raw-jpeg' in flags
    use_brightness = '--brightness' in flags

    # --- 亮度指令 ---
    if use_brightness:
        val = 0
        for f in flags:
            if f.startswith('--brightness='):
                val = int(f.split('=')[1])
                break
        send_brightness(ser, val)
        ser.close(); return

    image_file = None
    for a in args:
        if os.path.isfile(a): image_file = a; break

    if image_file:
        print(f'Loading image: {image_file}')
        try:
            if use_raw_jpeg:
                ok = send_raw_jpeg_via_serial(ser, image_file)
            else:
                ok = send_jpeg_via_serial(ser, image_file)
            if not ok:
                print('Transfer FAILED'); ser.close(); sys.exit(1)
        except Exception as e:
            print(f'Error: {e}'); ser.close(); sys.exit(1)
    else:
        print('No image file specified'); print(__doc__); ser.close(); sys.exit(1)

    ser.close()
    print('\nDone.')


if __name__ == '__main__':
    main()
