package handler

import (
	"encoding/json"
	"io"
	"net/http"
)

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

// Upload handles JPEG file upload and forwarding to base station.
// POST /upload?mac=8C:4F:00:53:A3:18
func (h *Handler) Upload(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	mac := r.URL.Query().Get("mac")
	if mac == "" {
		http.Error(w, "mac query parameter is required", http.StatusBadRequest)
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
	if err := h.Station.SendJpeg(mac, data); err != nil {
		http.Error(w, "send: "+err.Error(), http.StatusInternalServerError)
		return
	}
	json.NewEncoder(w).Encode(map[string]string{"status": "ok"})
}

// Brightness handles brightness command.
// POST /brightness  body: {"mac":"8C:4F:00:53:A3:18", "value": 5}
func (h *Handler) Brightness(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		MAC   string `json:"mac"`
		Value int    `json:"value"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "invalid json", http.StatusBadRequest)
		return
	}
	if req.MAC == "" {
		http.Error(w, "mac is required", http.StatusBadRequest)
		return
	}
	if err := h.Station.SendBrightness(req.MAC, req.Value); err != nil {
		http.Error(w, "send: "+err.Error(), http.StatusInternalServerError)
		return
	}
	json.NewEncoder(w).Encode(map[string]string{"status": "ok"})
}
