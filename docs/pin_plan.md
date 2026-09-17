# 引脚规划（ESP-12F / NodeMCU + ST7789V 240×240）

> 状态：**方案设计**（未实施，代码仍为当前 GPIO5 背光配置）
> ⚠ 背光驱动电路的最终设计（NPN 低边 + 基极下拉，active HIGH）见 `hardware_architecture.md` §7，该设计使 §3 的复位电平风险直接消除，实测步骤仅作为旧板验证手段保留。
> 供电架构：**24V/6A → T8A 保险 → 24V→19V 预稳压 → INA226 → 4×SW3518（现成模块）**；母线定 19V 是因为模块主开关 AON7534 只有 30V（24V 下仅 1.25×，准则要 ≥1.5×）。辅助 3V3 必须用 buck（19V 直降 LDO 仍会烧 4.6W），见 `hardware_architecture.md` §3–§6。
> 📌 **v5 纯充电站形态**：显示屏改为 0.96" I2C OLED → **TFT 的 DC/RST/MOSI/SCK + 背光共 5 个 GPIO（0/2/12/13/14）与背光电路退役**，本文 §1/§3/§4/§5 的推导保留作参考；实际只实施 **§2（I2C = GPIO4/5 唯一可行）** + 2 个按键脚（GPIO0/2）。
> 目标：屏幕 + 串口 + **I2C** 三者共存
> I2C 从设备：**BME280（0x76）+ INA226（0x40，19V 母线总输入计量）**，见 `hardware_architecture.md` §5；INA226 **不新增 GPIO**
> 约束来源：ESP8266EX Datasheet 启动 strapping、ESP8266 Arduino Core 3.1.2 实现、TFT_eSPI 2.5.43 源码

---

## 1. 结论：目标引脚表

| 功能 | GPIO | NodeMCU | 相对现状 |
|------|------|---------|---------|
| TFT DC | GPIO0 | D3 | 不变 |
| TFT RST | GPIO2 | D4 | 不变 |
| TFT SCLK | GPIO14 | D5 | 不变 |
| TFT MOSI | GPIO13 | D7 | 不变 |
| TFT CS | 接 GND | — | 不变 |
| **背光 PWM** | **GPIO12** | **D6** | ← 从 GPIO5(D1) 移来 |
| **I2C SDA** | **GPIO4** | **D2** | 新增（BME280 + INA226 共用） |
| **I2C SCL** | **GPIO5** | **D1** | 新增（接管原背光脚） |
| 串口 TX / RX（传图 115200） | GPIO1 / GPIO3 | D10 / D9 | 不变 |
| 空闲 | GPIO15 | D8 | 保留（注意启动约束） |
| 空闲 | GPIO16 | D0 | 保留（无中断、无 PWM） |
| 空闲 | A0 (TOUT) | A0 | 模拟输入，可用于光敏电阻等 |

改线只需动两根：**背光驱动线 D1 → D6**，**传感器接 D2/D1**。

```
              ST7789V 240×240
   ┌──────────────────────────────────────┐
   │ DC──D3  RST──D4  SCK──D5  SDA──D7    │
   │ CS──GND  VDD──3V3  LEDK──GND         │
   │ LEDA──[驱动级]──D6 (GPIO12, LOW=亮)  │
   └──────────────────────────────────────┘
   I2C 总线: SDA──D2(GPIO4)  SCL──D1(GPIO5)  + 4.7k 上拉到 3V3
            ├─ BME280  (0x76)
            └─ INA226  (0x40, 5mΩ 分流测 19V 母线总输入电流, VB 测 19V, VS 接 3V3)
```

---

## 2. 为什么 I2C 只能落在 GPIO4 + GPIO5

这是本方案的核心推导，不是随意选定：

**(a) I2C 在电气上要求两根线在空闲/复位时为高电平。**
总线必须外接上拉电阻（4.7k → 3V3），因此上位机尚未运行固件时，这两个引脚就已经被拉高。

**(b) ESP-12F 有 5 个"复位电平敏感"脚，被拉高会导致不启动或 flash 电压错误：**

| GPIO | 名称 | 复位要求 | 被 I2C 上拉拉的后果 |
|------|------|---------|--------------------|
| GPIO0 | GPIO0 | 高（低=下载模式） | 已作 TFT_DC，不可用 |
| GPIO2 | GPIO2 | 高 | 已作 TFT_RST，不可用 |
| GPIO15 | MTDO | **必须低** | ❌ 无法启动 |
| GPIO12 | MTDI | **必须低** | ❌ flash 供电切 1.8V，无法启动 |
| GPIO5 | — | 无要求 | ✅ 安全 |
| GPIO4 | — | 无要求 | ✅ 安全 |

**(c) 其余非 strapping 脚都已被占用或不可用：**

| GPIO | 排除原因 |
|------|---------|
| GPIO1 / GPIO3 | UART0 TX/RX，本工程传图通道（115200），不能动 |
| GPIO13 / GPIO14 | TFT SPI MOSI/SCLK |
| GPIO16 | ❌ **不能用 ESP8266 Wire 库**：`core_esp8266_si2c.cpp` 用 `GPES/GPEC/GPI` 的 `1 << pin` 位掩码操作引脚，寄存器只覆盖 GPIO0~15；GPIO16 走独立的 `GP16E/GP16O`，位掩码无效 |

**(d) 结论**：可用的非 strapping 空闲脚只剩 **GPIO4、GPIO5**。
→ **I2C = GPIO4 (SDA) + GPIO5 (SCL)** 是唯一可行组合。

> 副作用（正面的）：GPIO4 + GPIO5 恰好就是 ESP8266 Core 的 `Wire` 默认脚，因此 `Wire.begin()` 不传参也能工作，标准示例代码/传感器库可直接用。

---

## 3. 为什么背光搬到 GPIO12，以及必须验证的复位电平

I2C 拿走 GPIO4/5 后，剩余可放背光的脚只有 GPIO12 / GPIO15 / GPIO16：

| 候选 | PWM 调光 | 复位电平要求 | 评价 |
|------|---------|-------------|------|
| GPIO16 | ❌ `analogWrite` 不支持 pin 16 | 无 | 会丢失 1~10 档亮度调节，否决 |
| GPIO15 | ✅ | **必须低** | 与 GPIO12 同等风险，但 GPIO15 被拉高是"直接不启动"，更难救 |
| **GPIO12** | ✅ | **必须低**（MTDI，flash 电压选择） | ✅ 选定 |

**关键风险点**：GPIO12 = MTDI。复位时若被外部电路拉高，ESP8266 会把 VDD_SDIO 切到 1.8V，3.3V flash 无法工作 → 上电即挂（表现为串口无输出、无法烧录，需手动拉低 GPIO12 才能救回）。

背光驱动级（NPN/PNP 三极管）**是否存在基极上拉电阻，本仓库无原理图，属待确认项**。
- 若驱动级输入在悬空时被基极-发射极二极管钳到 ~0.7V 以下（典型 NPN 低边 + 串基极电阻、无上拉）→ **安全**，GPIO12 复位时为低。
- 若驱动级是 PNP 高边开关并带基极上拉（这类 active-LOW 电路很常见，也解释了现有 `LOW = 亮` 的极性）→ **复位时 GPIO12 被拉高 → 不可行**，必须改电路或换方案。

### 3.1 判定方法（实施前必做）

GPIO12 在固件运行前是**高阻输入**，所以复位瞬间该脚电平完全由驱动电路决定：

1. 只插 USB 供电，**不要让固件运行**（或直接拿掉 ESP 模块 / 拔掉 D6 跳线到驱动级的连接）。
2. 用万用表/示波器测 **驱动电路输入端**（即将来接 D6 的那根线）对 GND 电压。
3. 判定：
   - `≤ 0.4V` 或悬空可忽略 → ✅ 通过，方案可实施
   - `≈ 3.3V`（有上拉） → ❌ 需按 3.2 处理

更保险的做法：接好线后上电，示波器抓 GPIO12，在 `RST` 释放后的前 100ms 内必须保持低。

### 3.2 判定失败时的备选方案

| 方案 | 做法 | 代价 |
|------|------|------|
| B1 | 改驱动为 **NPN 低边**（LEDK→集电极，LEDA→3V3，去掉基极上拉），控制脚复位天然为低；同时 `TFT_BACKLIGHT_ON` 改为 `HIGH`，亮度映射表同步取反 | 改 1 处电路 + 改极性宏，最彻底 |
| B2 | 保留现驱动，去掉/改小基极上拉，确保复位时为低 | 需评估驱动级是否还能可靠关断 |
| B3 | 背光不用 PWM：控制脚改走 GPIO16（无 PWM），亮度改为软件开关或放弃调光 | 丢失 1~10 档亮度，`CMD_SET_BRIGHTNESS` 功能降级 |
| B4 | 放弃 I2C 或改用 1-Wire/单总线传感器（只用 1 个脚，可放 GPIO4，背光留在 GPIO5） | 无硬件改动，但传感器选型受限 |

---

## 4. 与 TFT_eSPI 的交互（已核对源码，确认无冲突）

| 检查项 | 结论 |
|--------|------|
| `TFT_BL 12` 是否被库正确驱动 | ✅ `TFT_eSPI.cpp` 中 `#if defined(TFT_BL) && defined(TFT_BACKLIGHT_ON)` → `pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);`，任何 GPIO 都走这条通用路径 |
| `TFT_CS -1` | ✅ `if (TFT_CS >= 0)` 守卫，跳过，不会 `pinMode` |
| `TFT_MISO` 未定义 | ✅ 被兜底为 `-1`，库不会把 GPIO12 配成 MISO 输入 |
| 但 `spi.begin()` 会占 GPIO12 | ⚠️ `libraries/SPI/SPI.cpp` 的 `SPIClass::begin()` 对 HSPI 执行 `pinMode(12, SPECIAL)`（HSPI MISO）。**本工程不使用 MISO**，且 `analogWrite()` 内部会 `pinMode(pin, OUTPUT)`（`__pinMode` 里 `GPF(pin) = GPFFS(GPFFS_GPIO(pin))` 把功能抢回 GPIO）→ 实际无影响。**前提：`analogWrite(TFT_BL, ...)` 必须在 `tft.init()` 之后调用**（现有代码正是如此，位于 `espnowReceiverInit()`） |
| 未来若启用屏读（`#define TFT_MISO 12`） | ❌ 会与背光冲突，届时应把 I2C/背光重新分配 |

---

## 5. 完整引脚可用性矩阵（规划后）

| GPIO | NodeMCU | 规划用途 | 复位约束 | 备注 |
|------|---------|---------|---------|------|
| 0 | D3 | TFT_DC | 需高（低=下载模式） | 输出用，正常 |
| 1 | D10 | UART TX | — | 传图 |
| 2 | D4 | TFT_RST | 需高 | 输出用，正常 |
| 3 | D9 | UART RX | — | 传图 |
| 4 | D2 | **I2C SDA** | 无 | 需 4.7k 上拉；BME280 + INA226 |
| 5 | D1 | **I2C SCL** | 无 | 需 4.7k 上拉；BME280 + INA226 |
| 12 | D6 | **背光 PWM** | **必须低** | 见 §3 验证 |
| 13 | D7 | TFT MOSI | 无 | |
| 14 | D5 | TFT SCLK | 无 | |
| 15 | D8 | 空闲 | **必须低** | 可作输出/按钮，不可加上拉 |
| 16 | D0 | 空闲 | 无 | 无中断、无 PWM、不能做 I2C |
| A0 | A0 | 空闲 | — | 唯一 ADC，0~1.0V（板载分压） |

---

## 6. 后续实施清单（本次不改）

### 软件

| 文件 | 改动 |
|------|------|
| `Cube/User_Setup.h` | `#define TFT_BL 12`；更新头部接线注释；补充 GPIO12 strapping 与 HSPI MISO 说明 |
| `Cube/src/main.h` | 增加 `#include <Wire.h>` 与 `i2cInit()`（`Wire.begin(4, 5); Wire.setClock(100000);`） |
| `Cube/src/espnow_display.ino` | 接收端 / 发送端 `setup()` 各调用一次 `i2cInit()` |
| `README.md` | 硬件小节：背光 GPIO5 → GPIO12，新增 I2C 行 |

> `espnow_receiver.cpp` 中 `analogWrite(TFT_BL, pwm)` 用的是宏，改 `User_Setup.h` 后自动跟随，无需改逻辑；亮度档位 1=暗 / 10=亮 的映射与极性保持不变。

### 硬件

1. 背光驱动输入端从 **D1 挪到 D6**（GPIO5 → GPIO12），驱动电路按 `hardware_architecture.md` §7 改为 NPN 低边 + 10k 基极下拉（active HIGH）。
2. 传感器 SDA → **D2**、SCL → **D1**，各加 4.7k 上拉到 3V3（模块自带则免）；BME280 与 INA226 并接同一总线。
3. INA226：IN+/IN− 四线 Kelvin 接外置 5mΩ/2512 分流器两端，分流串在 **19V 母线（预稳压输出、4 路分配节点上游）**；**VS 接 3V3，VB 接分流下游 19V**，严禁用母线给芯片供电（详见 `hardware_architecture.md` §5.1）。
3. 上电前先做 §3.1 的复位电平测量（若已按 §5 改为 NPN 低边 + 基极下拉，此项应天然通过）。

### 验证清单

- [ ] 复位瞬间 GPIO12 ≤ 0.4V（示波器/万用表）
- [ ] 连续断电重启 20 次，无启动失败（strapping 稳定性）
- [ ] `i2c_scanner` 能扫到 0x76 (BME280) 与 0x40 (INA226)，100kHz 无读写错误
- [ ] BME280 与 INA226 同总线互不干扰（连续 1Hz 读取 24h 无 NACK）
- [ ] 亮度 1~10 档逐档可调，方向正确（1 暗 / 10 亮）
- [ ] 串口传图（115200）+ ESP-NOW 转发不受影响，整图仍约 2s
- [ ] 刷图大负载下传感器读取不丢包（必要时把采样周期放到 ≥1s）

---

## 7. 风险登记

| 风险 | 触发条件 | 影响 | 缓解 |
|------|---------|------|------|
| GPIO12 复位被驱动级拉高 | 驱动级带上拉（PNP 高边常见） | **上电不启动**，需手动救砖 | §3.1 实测 + 方案 B1/B2 |
| GPIO12 被 `spi.begin()` 设为 HSPI MISO | — | 若在 `tft.init()` 后未重新 `pinMode` 则背光不亮 | 保证 `analogWrite` 在 `tft.init()` 之后（现状满足） |
| I2C 上拉不足 / 走线过长 | 无模块自带上拉、杜邦线 >30cm | 读传感器偶发失败 | 4.7k 上拉、100kHz、缩短走线（挂 2 个器件后总线电容仍很小） |
| 分流器/INA226 采样线引入开关噪声 | 4 路开关电流路径经过 Kelvin 感线 | 电流读数抖动/偏差 | 分流紧邻预稳压输出端子，输入电容置于分流下游，详见 `hardware_architecture.md` §5.2 |
| INA226 量程/Cal 用错 | 沿用旧文的 16.6V 档 / Cal=15812 | 电压截顶、电流读数错误 | 必须 32V 档 + Cal=2048（I_LSB 0.5mA，5mΩ），见 `hardware_architecture.md` §5.3 |
| Wire 为软件 bit-bang，与刷屏争 CPU | 刷图同时高频读传感器 | 掉帧/读失败 | 传感器采样周期 ≥1s，避开 `pushImage` 窗口 |
| 后续想用 GPIO15 做输出 | 误加上拉 | 无法启动 | 只做输出/接地按钮，禁止上拉 |
