package handler

import (
	"encoding/json"
	"io"
	"net/http"
)

type StationProvider interface {
	SendJpeg(data []byte) error
	SendBrightness(value int) error
	SetReceiver(mac string) error
	GetReceiver() string
}

// Handler holds dependencies for HTTP handlers.
type Handler struct {
	Station StationProvider
}

// MACs returns known receiver MACs and the active one.
// GET /api/macs
func (h *Handler) MACs(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodPost {
		var req struct {
			MAC string `json:"mac"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			http.Error(w, "invalid json", http.StatusBadRequest)
			return
		}
		if err := h.Station.SetReceiver(req.MAC); err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
	}
	json.NewEncoder(w).Encode(map[string]interface{}{
		"active": h.Station.GetReceiver(),
		"known":  []string{"8C:4F:00:53:A3:18", "EC:64:C9:C9:37:0C"},
	})
}

func New(station StationProvider) *Handler {
	return &Handler{Station: station}
}

// Upload handles JPEG file upload and forwarding to base station.
// POST /upload
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
	if err := h.Station.SendJpeg(data); err != nil {
		http.Error(w, "send: "+err.Error(), http.StatusInternalServerError)
		return
	}
	json.NewEncoder(w).Encode(map[string]string{"status": "ok"})
}

// Brightness handles brightness command.
// POST /brightness  body: {"value": 5}
func (h *Handler) Brightness(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		Value int `json:"value"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "invalid json", http.StatusBadRequest)
		return
	}
	if err := h.Station.SendBrightness(req.Value); err != nil {
		http.Error(w, "send: "+err.Error(), http.StatusInternalServerError)
		return
	}
	json.NewEncoder(w).Encode(map[string]string{"status": "ok"})
}
