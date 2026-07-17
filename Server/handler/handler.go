package handler

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"sync"
)

// imageStore 内存存储已上传的图片，key=uuid
var imageStore = struct {
	sync.RWMutex
	m map[string][]byte
}{m: make(map[string][]byte)}

type StationProvider interface {
	SendJpeg(mac string, data []byte) error
	SendBrightness(mac string, value int) error
	PortName() string
	Connected() bool
	Close() error
	Reopen(portName string) error
	RefreshPorts() ([]string, string, error)
}

// Handler holds dependencies for HTTP handlers.
type Handler struct {
	Station StationProvider
}

// MACs returns known receiver MACs.
// GET /api/macs
func (h *Handler) MACs(w http.ResponseWriter, r *http.Request) {
	json.NewEncoder(w).Encode(map[string]interface{}{
		"known": []string{"8C:4F:00:53:A3:18", "EC:64:C9:C9:37:0C"},
	})
}

// Ports returns available serial ports and current port.
// GET /api/ports - list ports
// POST /api/ports - switch to new port
func (h *Handler) Ports(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodGet {
		ports, current, err := h.Station.RefreshPorts()
		if err != nil {
			http.Error(w, "list ports: "+err.Error(), http.StatusInternalServerError)
			return
		}
		json.NewEncoder(w).Encode(map[string]interface{}{
			"ports":     ports,
			"active":    current,
			"connected": h.Station.Connected(),
		})
		return
	}
	if r.Method == http.MethodPost {
		var req struct {
			Port string `json:"port"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			http.Error(w, "invalid json", http.StatusBadRequest)
			return
		}
		if req.Port == "" {
			http.Error(w, "port is required", http.StatusBadRequest)
			return
		}
		if err := h.Station.Reopen(req.Port); err != nil {
			http.Error(w, "switch port: "+err.Error(), http.StatusInternalServerError)
			return
		}
		w.WriteHeader(http.StatusOK)
		json.NewEncoder(w).Encode(map[string]string{"status": "ok", "port": req.Port})
	}
}

// Disconnect closes the serial port connection.
// POST /api/disconnect
func (h *Handler) Disconnect(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if err := h.Station.Close(); err != nil {
		http.Error(w, "close: "+err.Error(), http.StatusInternalServerError)
		return
	}
	json.NewEncoder(w).Encode(map[string]string{"status": "ok"})
}

func New(station StationProvider) *Handler {
	return &Handler{Station: station}
}

// genUUID 生成随机 16 字节十六进制字符串
func genUUID() string {
	b := make([]byte, 16)
	rand.Read(b)
	return hex.EncodeToString(b)
}

// Upload 接收 JPEG 文件上传，返回图片 UUID（不发送到基站）。
// POST /api/upload  Content-Type: image/jpeg  body: <jpeg数据>
func (h *Handler) Upload(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	data, err := io.ReadAll(r.Body)
	if err != nil || len(data) == 0 {
		http.Error(w, "read body: "+err.Error(), http.StatusBadRequest)
		return
	}
	if len(data) < 64 || len(data) > 32768 {
		http.Error(w, "file size must be 64-32768 bytes", http.StatusBadRequest)
		return
	}
	// Check JPEG magic
	if len(data) < 3 || data[0] != 0xFF || data[1] != 0xD8 || data[2] != 0xFF {
		http.Error(w, "not a valid JPEG file", http.StatusBadRequest)
		return
	}

	uuid := genUUID()

	imageStore.Lock()
	imageStore.m[uuid] = data
	imageStore.Unlock()

	json.NewEncoder(w).Encode(map[string]interface{}{
		"uuid": uuid,
		"size": len(data),
	})
}

// Cmd 向指定接收机下发指令。
// POST /api/cmd  body: {"mac":"...", "cmd":"brightness", "value":5}
// POST /api/cmd  body: {"mac":"...", "cmd":"display", "uuid":"..."}
func (h *Handler) Cmd(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		MAC   string `json:"mac"`
		Cmd   string `json:"cmd"`
		Value int    `json:"value,omitempty"`
		UUID  string `json:"uuid,omitempty"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "invalid json", http.StatusBadRequest)
		return
	}
	if req.MAC == "" {
		http.Error(w, "mac is required", http.StatusBadRequest)
		return
	}
	if req.Cmd == "" {
		http.Error(w, "cmd is required (brightness|display)", http.StatusBadRequest)
		return
	}

	switch req.Cmd {
	case "brightness":
		if err := h.Station.SendBrightness(req.MAC, req.Value); err != nil {
			http.Error(w, "send: "+err.Error(), http.StatusInternalServerError)
			return
		}
		json.NewEncoder(w).Encode(map[string]string{"status": "ok"})

	case "display":
		if req.UUID == "" {
			http.Error(w, "uuid is required for display cmd", http.StatusBadRequest)
			return
		}
		imageStore.RLock()
		data, ok := imageStore.m[req.UUID]
		imageStore.RUnlock()
		if !ok {
			http.Error(w, "image not found", http.StatusNotFound)
			return
		}
		if err := h.Station.SendJpeg(req.MAC, data); err != nil {
			http.Error(w, "send: "+err.Error(), http.StatusInternalServerError)
			return
		}
		json.NewEncoder(w).Encode(map[string]string{"status": "ok"})

	default:
		http.Error(w, "unknown cmd: "+req.Cmd, http.StatusBadRequest)
	}
}
