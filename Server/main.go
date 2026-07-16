package main

import (
	"embed"
	"flag"
	"html/template"
	"io/fs"
	"log"
	"net/http"

	"espnow-server/handler"
	"espnow-server/serial"
)

//go:embed static/*
var staticFiles embed.FS

func main() {
	port := flag.String("port", "8080", "HTTP server port")
	serialPort := flag.String("serial", "", "Base station serial port (e.g. /dev/ttyUSB0)")
	listPorts := flag.Bool("list", false, "List available serial ports")
	flag.Parse()

	if *listPorts {
		ports, err := serial.ListPorts()
		if err != nil {
			log.Fatalf("list ports: %v", err)
		}
		log.Println("Available serial ports:")
		for _, p := range ports {
			log.Println(" ", p)
		}
		return
	}

	if *serialPort == "" {
		log.Fatal("--serial <port> is required (use --list to see available ports)")
	}

	// Connect to base station
	station, err := serial.Open(*serialPort)
	if err != nil {
		log.Fatalf("connect to %s: %v", *serialPort, err)
	}
	defer station.Close()
	log.Printf("Connected to base station on %s", *serialPort)

	// Setup HTTP
	h := handler.New(station)

	// API routes
	http.HandleFunc("/api/upload", h.Upload)
	http.HandleFunc("/api/brightness", h.Brightness)

	// Static files
	staticFS, _ := fs.Sub(staticFiles, "static")
	http.Handle("/", http.FileServer(http.FS(staticFS)))
	http.HandleFunc("/upload", func(w http.ResponseWriter, r *http.Request) {
		tmpl := template.Must(template.ParseFS(staticFiles, "static/upload.html"))
		tmpl.Execute(w, nil)
	})

	log.Printf("Server listening on :%s", *port)
	log.Fatal(http.ListenAndServe(":"+*port, nil))
}
