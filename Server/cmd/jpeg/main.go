// jpeg 测试程序：通过串口向基站发送 JPEG 图片
//
// 协议:
//   CMD_JPG_START(0x10) → CMD_JPG_DATA(0x11)×N → 等待30个ACK → CMD_IMG_END(0x03)
//
// 用法:
//   ./jpeg --port /dev/ttyUSB0 --mac 8C:4F:00:53:A3:18 --file /tmp/test_image.jpg
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
	CmdJpgStart  = 0x10
	CmdJpgData   = 0x11
	CmdImgEnd    = 0x03
	BaudRate     = 115200
	ChunkSize    = 512
	TotalStrips  = 30
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

// readACK 读取指定数量的 ACK (0x06)，带超时
func readACK(port serial.Port, count int, timeout time.Duration) (int, error) {
	deadline := time.Now().Add(timeout)
	received := 0
	buf := make([]byte, 1)
	for received < count && time.Now().Before(deadline) {
		n, err := port.Read(buf)
		if err != nil {
			return received, err
		}
		if n > 0 && buf[0] == 0x06 {
			received++
			if received%5 == 0 || received == count {
				log.Printf("  Strip %d/%d ✓", received, count)
			}
		}
	}
	return received, nil
}

// configBase 配置基站：等待 MAC 提示 → 发送 MAC → 确认
func configBase(port serial.Port, mac string, timeout time.Duration) error {
	data := readAll(port)
	if data != "" {
		log.Printf("📥 已有数据: %s", strings.TrimSpace(data))
	}

	if !strings.Contains(data, "Enter receiver MAC") && !strings.Contains(data, ">") {
		log.Print("⏳ 未发现 MAC 提示符，RTS 复位...")
		port.SetRTS(true)
		port.SetDTR(false)
		time.Sleep(300 * time.Millisecond)
		port.SetRTS(false)
		port.ResetInputBuffer()
		time.Sleep(2 * time.Second)
		data = readAll(port)
		if data != "" {
			log.Printf("📥 复位后: %s", strings.TrimSpace(data))
		}
	}

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

	log.Printf("📤 发送 MAC: %s", mac)
	if _, err := port.Write([]byte(mac + "\n")); err != nil {
		return fmt.Errorf("发送 MAC 失败: %w", err)
	}
	time.Sleep(2 * time.Second)
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

func main() {
	portName := flag.String("port", "", "基站串口 (例如 /dev/ttyUSB0)")
	mac := flag.String("mac", "", "接收机 MAC 地址")
	filePath := flag.String("file", "", "JPEG 文件路径")
	flag.Parse()

	if *portName == "" || *mac == "" || *filePath == "" {
		fmt.Println("用法: jpeg --port <串口> --mac <MAC> --file <JPEG文件>")
		fmt.Println()
		fmt.Println("示例:")
		fmt.Println("  jpeg --port /dev/ttyUSB0 --mac 8C:4F:00:53:A3:18 --file /tmp/test.jpg")
		fmt.Println("  jpeg --port /dev/ttyUSB0 --mac 8C:4F:00:53:A3:18 --file photo.jpg --quality 50")
		flag.PrintDefaults()
		os.Exit(1)
	}

	// 1. 读取 JPEG 文件
	jpgData, err := os.ReadFile(*filePath)
	if err != nil {
		log.Fatalf("❌ 读取文件失败: %v", err)
	}
	if len(jpgData) < 64 || len(jpgData) > 32768 {
		log.Fatalf("❌ JPEG 大小 %d 字节，必须在 64~32768 之间", len(jpgData))
	}
	// 检查 JPEG 文件头
	if len(jpgData) < 3 || jpgData[0] != 0xFF || jpgData[1] != 0xD8 || jpgData[2] != 0xFF {
		log.Fatal("❌ 不是有效的 JPEG 文件")
	}

	chunks := (len(jpgData) + ChunkSize - 1) / ChunkSize
	log.Printf("📷 JPEG: %s (%d bytes, %d chunks)", *filePath, len(jpgData), chunks)

	// 2. 打开串口
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

	// 3. 配置基站
	if err := configBase(port, *mac, 10*time.Second); err != nil {
		log.Fatalf("❌ 配置基站失败: %v", err)
	}

	// 4. 清空串口缓冲区（排除启动时的残留文本）
	time.Sleep(500 * time.Millisecond)
	port.ResetInputBuffer()
	log.Print("🧹 串口缓冲区已清空")

	// 5. 发送 JPEG
	log.Print("📤 发送 CMD_JPG_START...")
	sizeBytes := make([]byte, 2)
	binary.LittleEndian.PutUint16(sizeBytes, uint16(len(jpgData)))
	startPkt := Packet(CmdJpgStart, sizeBytes)
	log.Printf("   报文: % X", startPkt)
	if _, err := port.Write(startPkt); err != nil {
		log.Fatalf("❌ JPG_START 发送失败: %v", err)
	}
	time.Sleep(100 * time.Millisecond)

	// 发送数据分片（每 chunk 之间留 5ms 间隔）
	log.Print("📤 发送 CMD_JPG_DATA...")
	for offset := 0; offset < len(jpgData); offset += ChunkSize {
		end := offset + ChunkSize
		if end > len(jpgData) {
			end = len(jpgData)
		}
		chunk := jpgData[offset:end]
		pkt := Packet(CmdJpgData, chunk)
		n, err := port.Write(pkt)
		if err != nil {
			log.Fatalf("❌ JPG_DATA 发送失败 (offset=%d): %v", offset, err)
		}
		if n != len(pkt) {
			log.Fatalf("❌ JPG_DATA 写入不完整 (offset=%d): wrote %d/%d", offset, n, len(pkt))
		}
		time.Sleep(5 * time.Millisecond)
	}
	log.Print("✅ JPG_DATA 发送完成")

	// 等待 30 个 ACK
	log.Print("⏳ 等待 30 个 ACK...")
	startTime := time.Now()
	received, err := readACK(port, TotalStrips, 30*time.Second)
	elapsed := time.Since(startTime)
	if err != nil {
		log.Printf("⚠️  ACK 读取错误: %v", err)
	}
	if received < TotalStrips {
		log.Printf("⚠️  ACK 超时: 收到 %d/%d", received, TotalStrips)
	} else {
		log.Printf("✅ ACK 全部收到 (%d/%d, %v)", received, TotalStrips, elapsed)
	}

	// 发送 CMD_IMG_END
	log.Print("📤 发送 CMD_IMG_END...")
	endPkt := Packet(CmdImgEnd, nil)
	if _, err := port.Write(endPkt); err != nil {
		log.Fatalf("❌ IMG_END 发送失败: %v", err)
	}

	// 读取回复
	time.Sleep(500 * time.Millisecond)
	resp := readAll(port)
	if resp != "" {
		log.Printf("📥 基站回复: %s", strings.TrimSpace(resp))
	}

	log.Print("🎉 图片传输完成！")
}
