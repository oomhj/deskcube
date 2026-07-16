# 串口通信 API

> 基站与宿主机之间的 USB 串口通信协议。115200 baud, 8N1。

---

## 包结构

### 通用包头

所有串口包使用统一格式：

```
Byte:     0         1      2          3 ... N          N+1
         ┌─────────┬──────┬──────┬──────────────────┬──────────┐
         │  cmd    │ len_lo│len_hi│   payload        │  xor_sum │
         │  1B     │  1B   │  1B  │   len bytes      │  1B      │
         └─────────┴──────┴──────┴──────────────────┴──────────┘
```

- **cmd**: 命令字
- **len**: payload 长度（小端序，uint16）
- **payload**: 数据体
- **xor_sum**: 校验和，取值 `cmd ^ len_lo ^ len_hi ^ payload[0] ^ payload[1] ^ ...`
  - 校验失败时基站丢弃该包回空闲状态，无重传

---

## 命令

### 1. CMD_JPG_START (0x10)

开始 JPEG 图片传输。

**请求**：
```
cmd=0x10  len=2  payload=[totalSize(2B)]  xor_sum
```

| 字段 | 类型 | 说明 |
|------|------|------|
| totalSize | uint16 LE | JPEG 文件总字节数，范围 64~32768 |

**处理**：基站 malloc(totalSize) 缓冲区，进入接收状态 `S_JPG_RECV`。

---

### 2. CMD_JPG_DATA (0x11)

JPEG 数据分片。

**请求**：
```
cmd=0x11  len=N  payload=[chunk(NB)]  xor_sum
```

| 字段 | 类型 | 说明 |
|------|------|------|
| chunk | uint8[] | JPEG 数据分片，长度 ≤ 512 字节 |

**处理**：基站提取 payload 写入 jpgBuf，实时累加 XOR。校验失败丢弃包。

**注意**：最后一个 chunk 可以小于 512 字节。基站用 `readBytes()` 批量读取，留 1 字节给 XOR。

---

### 3. CMD_IMG_START (0x01)

ESP-NOW 控制包：通知接收机准备接收图片。

**请求**：
```
cmd=0x01  len=0  xor_sum
```

**处理**：基站调用 `sendStartPacket()` 发送 ESP-NOW `PKT_IMAGE_START`。

---

### 4. CMD_IMG_END (0x03)

ESP-NOW 控制包：通知接收机图片传输结束。

**请求**：
```
cmd=0x03  len=0  xor_sum
```

**处理**：基站调用 `sendEndPacket()` 发送 ESP-NOW `PKT_IMAGE_END`。

---

### 5. CMD_CMD (0x20)

指令转发：宿主机 → 基站 → ESP-NOW → 接收机。

**请求**：
```
cmd=0x20  len=2+N  payload=[cmdId(1B)][cmdLen(1B)][params(NB)]  xor_sum
```

| 字段 | 类型 | 说明 |
|------|------|------|
| cmdId | uint8 | 指令 ID |
| cmdLen | uint8 | 参数长度 |
| params | uint8[] | 参数数据，最多 4 字节 |

**支持指令**：

| cmdId | 名称 | 参数 | 说明 |
|-------|------|------|------|
| 0x01 | 设置亮度 | `[brightness(1B)]` | 1~10，80%~100% PWM |

**处理**：基站收到后通过 ESP-NOW `PKT_CMD` 转发到接收机。
接收机执行后 EEPROM 保存，重启后自动恢复。

---

## 应答

基站不逐包应答。完成 JPEG 接收 + ESP-NOW 转发后，回 30 个 `0x06`（ACK）字节：

```
宿主机 ←── 0x06 ×30 ── 基站
```

Python 通过 `wait_strip_ack()` 逐个读取这 30 个 ACK 确认传图完成。

超时时间由 `STRIP_ACK_TIMEOUT` 环境变量控制，默认 30 秒。

---

## 数据流示例

### JPEG 传图

```
宿主机 → CMD_JPG_START(totalSize=6554)
宿主机 → CMD_JPG_DATA(chunk 0, 512B)   [基站接收并校验 XOR]
宿主机 → CMD_JPG_DATA(chunk 1, 512B)
...
宿主机 → CMD_JPG_DATA(chunk 12, 250B)  [末尾 chunk]
                                      [基站收齐 → 解码 → ESP-NOW 转发]
宿主机 ←─── 0x06 ×30 ─────────────────  [30 个 ACK]
宿主机 → CMD_IMG_END
```

### 亮度指令

```
宿主机 → CMD_CMD(cmdId=0x01, params=[5])
                                      [基站 → ESP-NOW → 接收机]
                                      [接收机 analogWrite(PWM)]
                                      [接收机 EEPROM.save()]
宿主机 ←─── 0x06 ─────────────────────  [1 个 ACK]
```

---

## 错误处理

| 错误场景 | 基站行为 | 宿主机遇见 |
|----------|----------|-----------|
| XOR 校验失败 | 丢弃包，回 S_IDLE | ACK 超时 |
| chunk 命令字不为 0x11 | 丢弃整个 JPEG，回 S_IDLE | ACK 超时 |
| 30 秒内没收齐 JPEG | 释放 jpgBuf，回 S_IDLE | ACK 超时 |
| JPEG 大小超出 64~32768 | 拒绝，回 S_IDLE | ACK 超时 |
| malloc 失败 | 回 S_IDLE | ACK 超时 |
| ESP-NOW 发送失败 | 仍回 30 个 ACK（已尽力） | 接收机可能收不到图 |
