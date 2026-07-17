#!/usr/bin/env python3
"""
通过 HTTP API 下发亮度指令
用法: python3 set_brightness.py 8C:4F:00:53:A3:18 10
"""
import sys
import requests

BASE_URL = "http://localhost:8088"

def set_brightness(mac: str, value: int) -> bool:
    r = requests.post(f"{BASE_URL}/api/cmd", json={"mac": mac, "cmd": "brightness", "value": value})
    if r.ok:
        print(f"✅ 亮度已设置为: {value} (接收机: {mac})")
        return True
    print(f"❌ 亮度设置失败: {r.text}")
    return False

def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)

    mac = sys.argv[1].upper()
    value = int(sys.argv[2])

    if not set_brightness(mac, value):
        sys.exit(1)

if __name__ == "__main__":
    main()
