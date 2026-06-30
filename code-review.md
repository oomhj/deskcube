# 代码审查记录

## SmartConfig 模块

### Bug 1：第一次等待无超时（死循环风险）

**位置**：`src/clockV101.ino` `smart_config()` 函数，第一个 `while` 循环

```cpp
while (!WiFi.smartConfigDone())   // ← 用户不操作 App 则永远不退出
{
    for (uint8_t n = 0; n < 10; n++)
        PowerOn_Loading(50);      // ← 全是 delay()，看门狗可能超时
    Serial.print(".");
}
```

**影响**：用户不打开配网 App 时设备卡死，Watchdog 复位后继续卡死。

**建议**：加超时计数，120 秒无响应则 `WiFi.stopSmartConfig()` 并 return false，回退重试 EEPROM 中的旧 WiFi。

---

### Bug 2：SmartConfig 失败后无限重试

**位置**：`src/clockV101.ino` `connect_wifi()` 函数

```cpp
while (true)                      // ← 失败就无限循环
{
    bool success = smart_config();
    if (success == true) break;
}
```

**影响**：用户 App 配网输错密码 → WiFi 连接失败 → `smart_config()` 返回 false → 又从头 `WiFi.beginSmartConfig()`，每轮 30 秒，无限循环。

**建议**：限制重试次数（如 3 次），全部失败后回退到 EEPROM 旧 WiFi 或重启。

---

### Bug 3：首次开机 EEPROM 数据未初始化

**位置**：`src/clockV101.ino` `readWifiConf()` → `connect_wifi()`

**影响**：新芯片 EEPROM 全 `0xFF` → `WiFi.begin(垃圾数据)` → 60 秒后才 fallback 到 SmartConfig，浪费开机时间。

**建议**：`readWifiConf()` 中检测 `wifiConf.wifi_ssid[0]` 是否为 `\0` 或非 ASCII，直接跳过 WiFi 连接进 SmartConfig。

---

### Bug 4：类型 `uint8` 不一致

**位置**：`src/clockV101.ino:597`

```cpp
uint8 cnt = 1;  // 应为 uint8_t
```

**影响**：无功能影响（Arduino 有兼容别名），但代码风格不统一。

**建议**：改为 `uint8_t`。

---

## 已修复问题（历史记录）

### 1. DHT 传感器代码已清理

移除所有 DHT 对象、初始化、读取、显示分支（开发板无传感器）。

### 2. `#include <WIFIClient.h>` → `<WiFiClient.h>`

拼写错误修正。

### 3. 颜色条件永远为真

```cpp
// 修复前
if (0 <= wifiConf.frontColor && 65535)

// 修复后
if (wifiConf.frontColor != 0 && wifiConf.frontColor != 0xFFFF)
```

### 4. MQTT 回调重复分支

删除了重复的 `else if (0 == strcmp(topic, MQTT_TOPIC_LED))` 分支。

### 5. MQTT String 拼接导致堆碎片

`String message = ""` 改为 `char message[64]` + `memcpy`。

### 6. 未使用变量清理

删除 `Led_Flag`, `Led_State`, `now2`, `now3`, `LastTime3`, `LastTime4`, `LastTime5`。

### 7. `scrollBanner()` 全局 → 局部变量

`now1` 改为函数内局部变量。

### 8. `imgDisplay()` switch-case → 查表

将 5 个 ≈200 行的 switch-case 块改为 5 个数组查询表，减少约 400 行代码。

### 9. `if (0 <= wifiConf.frontColor && 65535)` 修正

永远为真，颜色读取无效。已改为排除 0（黑）和 0xFFFF（白）。

### 10. 死代码移除

`loop()` 中 `LastTime4` / `ds18b20` 相关死代码已删除。

### 11. 注释修正

"五条信息" → "8 条信息"，`frontColor`/`bgColor` 互换注释已更正。

### 12. TFT_eSPI 配置

- 驱动从 `ILI9341_DRIVER` 改为 `ST7789_DRIVER`
- 尺寸 240×240
- 引脚按硬件信息.md 接线图配置
- SPI 频率 20 MHz
- 字体精简为 FONT2 + FONT7 + SMOOTH_FONT 仅三组
- SD 库排除（`lib_ignore = SD, SDFS, ESP8266SdFat`）
- TJpg_Decoder 禁用 `TJPGD_LOAD_SD_LIBRARY`

---

## 代码整洁度问题（低优先级）

| # | 位置 | 说明 |
|---|------|------|
| 1 | `setup()` / `loop()` | 多处多余空行（DHT 代码移除后留空） |
| 2 | `change_color()` | 约 10 行被注释的旧代码未清理 |
| 3 | `handleRoot()` | `htmlCode +=` 拼接产生堆碎片，建议用 `F()` 宏或 `sendContent()` |
| 4 | `PowerOn_Loading()` | 使用 `delay()` 阻塞，不喂 WDT，有复位风险 |
| 5 | `getCityCode()` / `getCityWeater()` | 函数名 `Weater` 拼写错误（应为 `Weather`） |
