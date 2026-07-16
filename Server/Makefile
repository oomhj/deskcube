.PHONY: build-linux build-darwin clean run

build-linux:
	GOOS=linux GOARCH=amd64 go build -o server-linux-x64 .

build-darwin:
	go build -o server-darwin-arm64 .

run: build-darwin
	./server-darwin-arm64 --serial /dev/cu.usbserial-130 --port 8080

clean:
	rm -f server-linux-x64 server-darwin-arm64
