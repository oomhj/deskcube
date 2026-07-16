// brightness 测试程序：配置接收机 MAC → 通过串口向基站发送亮度调节指令
//
// 用法:
//   ./brightness --port /dev/ttyUSB0 --mac 8C:4F:00:53:A3:18 --value 5
package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"log"
	"os"
	"strings"
	"time"

	"go.bug.st/serial"
)

const (
	CmdCmd           = 0x20
	CmdSetBrightness = 0x01
	BaudRate         = 115200
)

// Packet 构建串口协议包（cmd + len + payload + xor）
func Packet(cmd byte, payload []byte) []byte {
	pkt := make([]byte, 3+len(payload)+1)
	pkt[0] = cmd
	binary.LittleEndian.PutUint16(pkt[1:3], uint16(len(payload)))
	copy(pkt[3:], payload)
	xor := cmd ^ byte(len(payload)&0xFF) ^ byte((len(payload)>>8)&0xFF)
	for _, b := range payload {
		xor ^= b
	}
	pkt[len(pkt)-1] = xor
	return pkt
}

// readAll 读取串口所有可用数据
func readAll(port serial.Port) string {
	buf := make([]byte, 4096)
	n, _ := port.Read(buf)
	if n > 0 {
		return string(buf[:n])
	}
	return ""
}

// configBase 配置基站：等待 MAC 提示 → 发送 MAC → 确认
// 与 Python config_base() 逻辑一致：
//   1. 先读已有数据
//   2. 无提示则 RTS 复位重试
//   3. 轮询等待提示符
//   4. 发送 MAC
//   5. 验证 "Using MAC" 确认
func configBase(port serial.Port, mac string, timeout time.Duration) error {
	// 1. 先读已有数据（设备可能已经在上次操作后等待输入）
	data := readAll(port)
	if data != "" {
		log.Printf("📥 已有数据: %s", strings.TrimSpace(data))
	}

	// 2. 没找到 MAC 提示 → RTS 复位重试
	if !strings.Contains(data, "Enter receiver MAC") && !strings.Contains(data, ">") {
		log.Print("⏳ 未发现 MAC 提示符，RTS 复位 NodeMCU...")
		// NodeMCU: RTS→RST, DTR→GPIO0
		port.SetRTS(true)   // RST low → 复位
		port.SetDTR(false)  // GPIO0 high → 正常启动
		time.Sleep(300 * time.Millisecond)
		port.SetRTS(false)  // 释放复位
		port.ResetInputBuffer()
		time.Sleep(2 * time.Second)

		data = readAll(port)
		if data != "" {
			log.Printf("📥 复位后: %s", strings.TrimSpace(data))
		}
	}

	// 3. 轮询等待 MAC 提示符
	deadline := time.Now().Add(timeout)
	prompted := strings.Contains(data, "Enter receiver MAC") || strings.Contains(data, ">")
	for !prompted && time.Now().Before(deadline) {
		chunk := readAll(port)
		if chunk != "" {
			log.Printf("📥 %s", strings.TrimSpace(chunk))
			if strings.Contains(chunk, "Enter receiver MAC") || strings.Contains(chunk, ">") {
				prompted = true
				break
			}
		}
		time.Sleep(200 * time.Millisecond)
	}
	if !prompted {
		return fmt.Errorf("未收到 MAC 提示符（超时 %v）", timeout)
	}

	// 4. 发送 MAC
	log.Printf("📤 发送 MAC: %s", mac)
	if _, err := port.Write([]byte(mac + "\n")); err != nil {
		return fmt.Errorf("发送 MAC 失败: %w", err)
	}
	time.Sleep(2 * time.Second)

	// 5. 验证确认
	resp := readAll(port)
	if resp != "" {
		log.Printf("📥 %s", strings.TrimSpace(resp))
	}
	if !strings.Contains(resp, "Using MAC") {
		return fmt.Errorf("MAC 未被接受，回复: %s", strings.TrimSpace(resp))
	}
	log.Print("✅ MAC 设置成功")
	return nil
}

// sendBrightness 发送亮度指令（不发 ACK，固件不回复）
func sendBrightness(port serial.Port, value int) error {
	payload := []byte{0x01, 0x01, byte(value)}
	pkt := Packet(CmdCmd, payload)
	log.Printf("📤 发送亮度指令: value=%d", value)
	log.Printf("   报文: % X", pkt)
	_, err := port.Write(pkt)
	return err
}

func main() {
	portName := flag.String("port", "", "基站串口 (例如 /dev/ttyUSB0)")
	mac := flag.String("mac", "", "接收机 MAC 地址 (例如 8C:4F:00:53:A3:18)")
	value := flag.Int("value", 5, "亮度值 1~10")
	flag.Parse()

	if *portName == "" || *mac == "" {
		fmt.Println("用法: brightness --port <串口> --mac <MAC> --value <1-10>")
		fmt.Println()
		fmt.Println("示例:")
		fmt.Println("  brightness --port /dev/ttyUSB0 --mac 8C:4F:00:53:A3:18 --value 7")
		flag.PrintDefaults()
		os.Exit(1)
	}
	if *value < 1 || *value > 10 {
		log.Fatalf("亮度值必须在 1~10 之间，收到: %d", *value)
	}

	// 1. 打开串口
	mode := &serial.Mode{
		BaudRate: BaudRate,
		DataBits: 8,
		Parity:   serial.NoParity,
		StopBits: serial.OneStopBit,
	}
	port, err := serial.Open(*portName, mode)
	if err != nil {
		log.Fatalf("❌ 打开串口失败: %v", err)
	}
	defer port.Close()
	port.SetReadTimeout(500 * time.Millisecond)
	log.Printf("✅ 串口 %s 已打开", *portName)

	// 2. 配置基站（等待 MAC 提示符 → 发送 MAC）
	if err := configBase(port, *mac, 10*time.Second); err != nil {
		log.Fatalf("❌ 配置基站失败: %v", err)
	}

	// 3. 发送亮度指令
	if err := sendBrightness(port, *value); err != nil {
		log.Fatalf("❌ 亮度指令发送失败: %v", err)
	}
	log.Print("✅ 亮度指令已发送")

	// 4. 读取回复
	time.Sleep(500 * time.Millisecond)
	resp := readAll(port)
	if resp != "" {
		log.Printf("📥 基站回复: %s", strings.TrimSpace(resp))
	} else {
		log.Print("ℹ️  基站无回复（固件不回复 ACK，属正常现象）")
	}

	log.Print("🎉 完成！")
}
