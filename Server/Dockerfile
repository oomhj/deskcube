FROM golang:1.24-alpine AS builder
WORKDIR /build
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 go build -o server .

FROM alpine:3.19
RUN apk add --no-cache ca-certificates
COPY --from=builder /build/server /server
EXPOSE 8080
ENTRYPOINT ["/server"]
