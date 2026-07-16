#!/usr/bin/env python3
"""
通过 HTTP API 设置接收机和亮度
用法: python3 set_brightness.py 8C:4F:00:53:A3:18 10
"""
import sys
import requests

BASE_URL = "http://localhost:8088"

def set_receiver(mac: str) -> bool:
    r = requests.post(f"{BASE_URL}/api/macs", json={"mac": mac})
    if r.ok:
        print(f"✅ 接收机已切换: {mac}")
        return True
    print(f"❌ 接收机切换失败: {r.text}")
    return False

def set_brightness(value: int) -> bool:
    r = requests.post(f"{BASE_URL}/api/brightness", json={"value": value})
    if r.ok:
        print(f"✅ 亮度已设置为: {value}")
        return True
    print(f"❌ 亮度设置失败: {r.text}")
    return False

def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    
    mac = sys.argv[1].upper()
    value = int(sys.argv[2])
    
    if not set_receiver(mac):
        sys.exit(1)
    if not set_brightness(value):
        sys.exit(1)

if __name__ == "__main__":
    main()
