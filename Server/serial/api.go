// Package serial implements the ESP-NOW base station serial protocol.
//
// Protocol: https://github.com/walkingsky/esp8266_weather_clock/blob/lcd/docs/serial_api.md
package serial

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
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
	port    serial.Port
	timeout time.Duration
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
	time.Sleep(500 * time.Millisecond) // let ESP boot
	return &Station{port: port, timeout: defaultTimeout}, nil
}

// Close closes the serial connection.
func (s *Station) Close() error { return s.port.Close() }

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
func (s *Station) SendBrightness(value int) error {
	if value < 1 || value > 10 {
		return errors.New("brightness must be 1-10")
	}
	payload := []byte{CmdSetBrightness, 0x01, byte(value)}
	if err := s.write(CmdCmd, payload); err != nil {
		return fmt.Errorf("cmd: %w", err)
	}
	// Expect 1 ACK
	return s.readAck(1)
}

// ListPorts returns available serial ports.
func ListPorts() ([]string, error) {
	ports, err := serial.GetPortsList()
	if err != nil {
		return nil, err
	}
	return ports, nil
}
