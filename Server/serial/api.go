// Package serial implements the ESP-NOW base station serial protocol.
//
// Protocol: https://github.com/walkingsky/esp8266_weather_clock/blob/lcd/docs/serial_api.md
package serial

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"strings"
	"time"

	"go.bug.st/serial"
)

// Protocol constants matching the firmware
const (
	CmdImgStart  byte = 0x01
	CmdImgEnd    byte = 0x03
	CmdJpgStart  byte = 0x10
	CmdJpgData   byte = 0x11
	CmdCmd       byte = 0x20

	TotalStrips  = 30
	ChunkSize    = 512
	BaudRate     = 115200

	// Command IDs
	CmdSetBrightness byte = 0x01

	defaultTimeout = 30 * time.Second
)

// Packet builds serial packet with XOR checksum.
func Packet(cmd byte, payload []byte) []byte {
	pkt := make([]byte, 3+len(payload)+1)
	pkt[0] = cmd
	binary.LittleEndian.PutUint16(pkt[1:3], uint16(len(payload)))
	copy(pkt[3:], payload)
	// XOR checksum
	xor := cmd ^ byte(len(payload)&0xFF) ^ byte((len(payload)>>8)&0xFF)
	for _, b := range payload {
		xor ^= b
	}
	pkt[len(pkt)-1] = xor
	return pkt
}

// Station manages serial connection to the base station.
type Station struct {
	port       serial.Port
	portName   string
	timeout    time.Duration
	ReceiverMAC string
}

// Open connects to the base station via serial port.
func Open(portName string) (*Station, error) {
	mode := &serial.Mode{
		BaudRate: BaudRate,
		DataBits: 8,
		Parity:   serial.NoParity,
		StopBits: serial.OneStopBit,
	}
	port, err := serial.Open(portName, mode)
	if err != nil {
		return nil, fmt.Errorf("open serial %s: %w", portName, err)
	}
	port.SetReadTimeout(1 * time.Second)
	// NodeMCU: RTS→RST (复位), DTR→GPIO0 (启动模式)
	// RTS high → RST low → 复位; RTS low → RST high → 运行
	port.SetRTS(true)   // 复位
	port.SetDTR(false)  // GPIO0 high → 正常启动
	time.Sleep(300 * time.Millisecond)
	port.SetRTS(false)  // 释放复位
	time.Sleep(2 * time.Second)
	port.ResetInputBuffer()
	return &Station{port: port, portName: portName, timeout: defaultTimeout}, nil
}

// SetReceiver resets the base station and sets the receiver MAC.
func (s *Station) SetReceiver(mac string) error {
	// NodeMCU: RTS→RST, DTR→GPIO0
	s.port.SetRTS(true)   // 复位
	s.port.SetDTR(false)  // GPIO0 high → 正常启动
	time.Sleep(300 * time.Millisecond)
	s.port.SetRTS(false)  // 释放复位
	s.port.ResetInputBuffer()
	time.Sleep(2 * time.Second)

	// Read until MAC prompt
	buf := make([]byte, 512)
	deadline := time.Now().Add(10 * time.Second)
	prompted := false
	for time.Now().Before(deadline) {
		n, _ := s.port.Read(buf)
		if n > 0 {
			text := string(buf[:n])
			if strings.Contains(text, "Enter receiver MAC") || strings.Contains(text, ">") {
				prompted = true
				break
			}
		}
		time.Sleep(200 * time.Millisecond)
	}
	if !prompted {
		return errors.New("no MAC prompt from base station")
	}
	// Send MAC
	s.port.Write([]byte(mac + "\n"))
	time.Sleep(2 * time.Second)
	// Verify
	n, _ := s.port.Read(buf)
	if !strings.Contains(string(buf[:n]), "Using MAC") {
		return errors.New("MAC not accepted")
	}
	s.ReceiverMAC = mac
	return nil
}

func (s *Station) GetReceiver() string { return s.ReceiverMAC }

// Close closes the serial connection.
func (s *Station) Close() error { return s.port.Close() }

// PortName returns the current port name.
func (s *Station) PortName() string { return s.portName }

// Reopen closes the current connection and opens a new serial port.
func (s *Station) Reopen(portName string) error {
	s.port.Close()
	newStation, err := Open(portName)
	if err != nil {
		return err
	}
	*s = *newStation
	return nil
}

// write sends a serial packet.
func (s *Station) write(cmd byte, payload []byte) error {
	pkt := Packet(cmd, payload)
	_, err := s.port.Write(pkt)
	return err
}

// readAck reads ACK bytes. Returns when count bytes received or timeout.
func (s *Station) readAck(count int) error {
	deadline := time.Now().Add(s.timeout)
	received := 0
	buf := make([]byte, 1)
	for received < count && time.Now().Before(deadline) {
		n, err := s.port.Read(buf)
		if err != nil && err != io.EOF {
			return err
		}
		if n > 0 && buf[0] == 0x06 {
			received++
		}
	}
	if received < count {
		return fmt.Errorf("ack timeout: got %d/%d", received, count)
	}
	return nil
}

// SendJpeg sends a JPEG file to the base station.
// Returns once all 30 ACKs are received (image sent via ESP-NOW).
func (s *Station) SendJpeg(data []byte) error {
	// CMD_JPG_START
	sizeBytes := make([]byte, 2)
	binary.LittleEndian.PutUint16(sizeBytes, uint16(len(data)))
	if err := s.write(CmdJpgStart, sizeBytes); err != nil {
		return fmt.Errorf("jpg start: %w", err)
	}
	time.Sleep(100 * time.Millisecond)

	// CMD_JPG_DATA chunks
	for offset := 0; offset < len(data); offset += ChunkSize {
		end := offset + ChunkSize
		if end > len(data) {
			end = len(data)
		}
		if err := s.write(CmdJpgData, data[offset:end]); err != nil {
			return fmt.Errorf("jpg data at %d: %w", offset, err)
		}
	}
	// Wait for 30 ACKs
	if err := s.readAck(TotalStrips); err != nil {
		return fmt.Errorf("ack: %w", err)
	}
	// CMD_IMG_END
	if err := s.write(CmdImgEnd, nil); err != nil {
		return fmt.Errorf("img end: %w", err)
	}
	return nil
}

// SendBrightness sends a brightness command (1-10).
// Note: firmware does not send ACK for command packets.
func (s *Station) SendBrightness(value int) error {
	if value < 1 || value > 10 {
		return errors.New("brightness must be 1-10")
	}
	payload := []byte{CmdSetBrightness, 0x01, byte(value)}
	if err := s.write(CmdCmd, payload); err != nil {
		return fmt.Errorf("cmd: %w", err)
	}
	return nil
}

// ListPorts returns available serial ports.
func ListPorts() ([]string, error) {
	ports, err := serial.GetPortsList()
	if err != nil {
		return nil, err
	}
	return ports, nil
}

// RefreshPorts returns available serial ports and the currently connected one.
func (s *Station) RefreshPorts() ([]string, string, error) {
	ports, err := serial.GetPortsList()
	if err != nil {
		return nil, "", err
	}
	return ports, s.portName, nil
}
