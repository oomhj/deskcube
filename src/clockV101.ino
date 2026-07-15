//**********************************************************************
// 本程序是网上开源的，我做了整理规范，修改了部分内容，添加了注释，编写了教程。

//*210409:
//**********************************************************************
#include "main.h"

#define VERSION "V101"



void readWifiConf() // 读取wifi配置
{
  // Read wifi conf from flash
  for (int i = 0; i < sizeof(wifiConf); i++)
  {
    ((char *)(&wifiConf))[i] = char(EEPROM.read(i));
  }
  // Make sure that there is a 0
  // that terminatnes the c string
  // if memory is not initalized yet.
  wifiConf.cstr_terminator = 0;
}

void writeWifiConf() // 保存wifi配置
{
  for (int i = 0; i < sizeof(wifiConf); i++)
  {
    EEPROM.write(i, ((char *)(&wifiConf))[i]);
  }
  EEPROM.commit();
}

// ========== 默认 WiFi（首次开机 / EEPROM 数据无效时使用）==========
#define DEFAULT_WIFI_SSID     "Jason-home"
#define DEFAULT_WIFI_PASSWORD "admin1234"

void connect_wifi() // 联网
{
  // 检查 EEPROM 中的 WiFi 是否有效，无效则用默认值
  if (wifiConf.wifi_ssid[0] == 0 || wifiConf.wifi_ssid[0] == (char)0xFF)
  {
    Serial.println("EEPROM WiFi 无效，使用默认 WiFi");
    strcpy(wifiConf.wifi_ssid, DEFAULT_WIFI_SSID);
    strcpy(wifiConf.wifi_password, DEFAULT_WIFI_PASSWORD);
    writeWifiConf();
  }

  Serial.println("\n========== WiFi 连接开始 ==========");
  Serial.print("目标 SSID: ");
  Serial.println(wifiConf.wifi_ssid);
  Serial.print("WiFi 模式: ");
  if (WiFi.getMode() & WIFI_STA) Serial.print("STA ");
  if (WiFi.getMode() & WIFI_AP) Serial.print("AP ");
  Serial.println();

  WiFi.begin(wifiConf.wifi_ssid, wifiConf.wifi_password);

  uint8_t cnt = 0;
  unsigned long lastStatusTime = millis();
  while (WiFi.status() != WL_CONNECTED)
  {
    for (uint8_t n = 0; n < 10; n++)
      PowerOn_Loading(50);

    cnt = cnt + 1;

    // 每 5 秒输出详细状态
    if (millis() - lastStatusTime > 5000)
    {
      int wlStatus = WiFi.status();
      Serial.print("[");
      Serial.print(cnt);
      Serial.print("/");
      Serial.print(wifi_connect_cnt);
      Serial.print("] WiFi 状态: ");
      switch (wlStatus)
      {
        case WL_IDLE_STATUS:     Serial.println("空闲 (WL_IDLE_STATUS)"); break;
        case WL_NO_SSID_AVAIL:   Serial.println("SSID 不可见 (WL_NO_SSID_AVAIL)"); break;
        case WL_SCAN_COMPLETED:  Serial.println("扫描完成 (WL_SCAN_COMPLETED)"); break;
        case WL_CONNECTED:       Serial.println("已连接 (WL_CONNECTED)"); break;
        case WL_CONNECT_FAILED:  Serial.println("连接失败 (WL_CONNECT_FAILED)"); break;
        case WL_CONNECTION_LOST: Serial.println("连接丢失 (WL_CONNECTION_LOST)"); break;
        case WL_DISCONNECTED:    Serial.println("断开 (WL_DISCONNECTED)"); break;
        default:                 Serial.printf("未知 (%d)\n", wlStatus); break;
      }
      lastStatusTime = millis();
    }

    if (cnt > wifi_connect_cnt)
    {
      Serial.println("\n✗ WiFi 连接超时!");
      Serial.println("  可能原因:");
      Serial.println("  1. SSID 或密码错误");
      Serial.println("  2. 路由器不在 2.4G 频段");
      Serial.println("  3. 信号太弱");
      Serial.print("  SSID: ");
      Serial.println(wifiConf.wifi_ssid);
      break;
    }
  }

  if (cnt > wifi_connect_cnt)
  {
    SmartConfigStatus = 0;
    while (true)
    {
      bool success = smart_config();
      if (success == true)
        break;
    }
  }

  SmartConfigStatus = 4;
  wifiConnected = true;

  while (loadNum < 194)
    PowerOn_Loading(1);

  Serial.println("\n========== WiFi 连接成功 ==========");
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());
  Serial.print("IP:   ");
  Serial.println(WiFi.localIP());
  Serial.print("RSSI: ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");
  local_IP = WiFi.localIP().toString();
}

void digitalClockDisplay() // 时间显示
{
  clk.setColorDepth(8);

  //--------------------中间时间区显示开始--------------------
  // 时分
  clk.createSprite(140, 48); // 创建Sprite，先在Sprite内存中画点，然后将内存中的点一次推向屏幕，这样刷新比较快
  clk.fillSprite(bgColor);   // 背景色
  // clk.loadFont(FxLED_48);
  clk.setTextDatum(CC_DATUM);              // 显示对齐方式
  clk.setTextColor(frontColor, bgColor);   // 文本的前景色和背景色
  clk.drawString(hourMinute(), 70, 24, 7); // 绘制时和分
  // clk.unloadFont();
  clk.pushSprite(28, 40); // Sprite中内容一次推向屏幕
  clk.deleteSprite();     // 删除Sprite

  // 秒
  clk.createSprite(40, 32);
  clk.fillSprite(bgColor);
  clk.loadFont(FxLED_32); // 加载字体
  clk.setTextDatum(CC_DATUM);
  clk.setTextColor(frontColor, bgColor);
  clk.drawString(num2str(second()), 20, 16);
  clk.unloadFont(); // 卸载字体
  clk.pushSprite(170, 60);
  clk.deleteSprite();
  //--------------------中间时间区显示结束--------------------

  //--------------------底部时间区显示开始--------------------
  clk.loadFont(ZdyLwFont_20); // 加载汉字字体

  // 星期
  clk.createSprite(58, 32);
  clk.fillSprite(bgColor);
  clk.setTextDatum(CC_DATUM);
  clk.setTextColor(frontColor, bgColor);
  clk.drawString(week(), 29, 16); // 周几
  clk.pushSprite(1, 168);
  clk.deleteSprite();

  // 月日
  clk.createSprite(98, 32);
  clk.fillSprite(bgColor);
  clk.setTextDatum(CC_DATUM);
  clk.setTextColor(frontColor, bgColor);
  clk.drawString(monthDay(), 49, 16); // 几月几日
  clk.pushSprite(61, 168);
  clk.deleteSprite();

  clk.unloadFont(); // 卸载字体
  //--------------------底部时间区显示结束--------------------
}

String week() // 星期
{
  String wk[7] = {"日", "一", "二", "三", "四", "五", "六"};
  String s = "周" + wk[weekday() - 1];
  return s;
}

String monthDay() // 月日
{
  String s = String(month());
  s = s + "月" + day() + "日";
  return s;
}

String hourMinute() // 时分
{
  String s = num2str(hour());
  s = s + ":" + num2str(minute());
  return s;
}

String num2str(int digits) // 数字转换成字符串，保持2位显示
{
  String s = "";
  if (digits < 10)
    s = s + "0";
  s = s + digits;
  return s;
}

void printDigits(int digits) // 打印时间数据
{
  Serial.print(":");
  if (digits < 10) // 打印两位数字
    Serial.print('0');
  Serial.print(digits);
}

time_t getNtpTime() // 获取NTP时间
{
  IPAddress ntpServerIP; // NTP服务器的IP地址

  while (Udp.parsePacket() > 0)
    ;                                          // 之前的数据没有处理的话一直等待 discard any previously received packets
  WiFi.hostByName(ntpServerName, ntpServerIP); // 从网站名获取IP地址

  sendNTPpacket(ntpServerIP); // 发送数据包
  uint32_t beginWait = millis();
  while (millis() - beginWait < 1500)
  {
    int size = Udp.parsePacket(); // 接收数据
    if (size >= NTP_PACKET_SIZE)
    {
      Serial.println("Receive NTP Response");
      Udp.read(packetBuffer, NTP_PACKET_SIZE); // 从缓冲区读取数据

      unsigned long secsSince1900;
      secsSince1900 = (unsigned long)packetBuffer[40] << 24;
      secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
      secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
      secsSince1900 |= (unsigned long)packetBuffer[43];
      return secsSince1900 - 2208988800UL + timeZone * SECS_PER_HOUR;
    }
  }
  Serial.println("No NTP Response :-(");
  return 0; // 没获取到数据的话返回0
}

void sendNTPpacket(IPAddress &address) // 发送数据包到NTP服务器
{
  memset(packetBuffer, 0, NTP_PACKET_SIZE); // 缓冲区清零

  packetBuffer[0] = 0b11100011; // LI, Version, Mode   填充缓冲区数据
  packetBuffer[1] = 0;          // Stratum, or type of clock
  packetBuffer[2] = 6;          // Polling Interval
  packetBuffer[3] = 0xEC;       // Peer Clock Precision
  packetBuffer[12] = 49;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 49;
  packetBuffer[15] = 52;

  Udp.beginPacket(address, 123);            // NTP服务器端口123
  Udp.write(packetBuffer, NTP_PACKET_SIZE); // 发送udp数据
  Udp.endPacket();                          // 发送结束
}

bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) // 显示回调函数
{
  if (y >= tft.height())
    return 0;
  tft.pushImage(x, y, w, h, bitmap);
  return 1;
}

void getCityCode() // 发送HTTP请求并且将服务器响应通过串口输出
{
  String citycode = wifiConf.city_id;
  if (citycode.startsWith("101") && citycode.length() == 9) // 判断配置的citycode是否可用，不可用就通过天气网的geo获取
  {
    cityCode = citycode;
    getCityWeater(); // 获取天气信息
    return;
  }
  String URL = "http://wgeo.weather.com.cn/ip/?_=" + String(now());

  httpClient.begin(wifiClient, URL);  // 配置请求地址。此处也可以不使用端口号和PATH而单纯的
  httpClient.setUserAgent("esp8266"); // 用户代理版本，其实没什么用 最重要是后端服务器支持
  // httpClient.setUserAgent("Mozilla/5.0 (iPhone; CPU iPhone OS 11_0 like Mac OS X) AppleWebKit/604.1.38 (KHTML, like Gecko) Version/11.0 Mobile/15A372 Safari/604.1");//设置请求头中的User-Agent
  httpClient.addHeader("Referer", "http://www.weather.com.cn/");

  int httpCode = httpClient.GET(); // 启动连接并发送HTTP请求
  Serial.print("Send GET request to URL: ");
  Serial.println(URL);

  if (httpCode == HTTP_CODE_OK)
  { // 如果服务器响应OK则从服务器获取响应体信息并通过串口输出
    String str = httpClient.getString();
    int aa = str.indexOf("id=");
    if (aa > -1)
    {                                               // 应答包里找到ID了
      cityCode = str.substring(aa + 4, aa + 4 + 9); // 9位长度
      Serial.println(cityCode);
      getCityWeater(); // 获取天气信息
      LastTime2 = millis();
    }
    else
    { // 没有找到ID
      Serial.println("获取城市代码失败");
    }
  }
  else
  {
    Serial.println("请求城市代码错误：");
    Serial.println(httpCode);
  }

  httpClient.end(); // 关闭与服务器连接
}

void getCityWeater() // 获取城市天气
{
  String URL = "http://d1.weather.com.cn/weather_index/" + cityCode + ".html?_=" + String(now());

  httpClient.begin(wifiClient, URL);  // 配置请求地址。
  httpClient.setUserAgent("esp8266"); // 用户代理版本，其实没什么用 最重要是后端服务器支持
  // httpClient.setUserAgent("Mozilla/5.0 (iPhone; CPU iPhone OS 11_0 like Mac OS X) AppleWebKit/604.1.38 (KHTML, like Gecko) Version/11.0 Mobile/15A372 Safari/604.1");//设置请求头中的User-Agent
  httpClient.addHeader("Referer", "http://www.weather.com.cn/");

  int httpCode = httpClient.GET(); // 启动连接并发送HTTP请求
  Serial.print("Send GET request to URL: ");
  Serial.println(URL);

  if (httpCode == HTTP_CODE_OK)
  { // 如果服务器响应OK则从服务器获取响应体信息并通过串口输出
    String str = httpClient.getString();
    int indexStart = str.indexOf("weatherinfo\":"); // 寻找起始和结束位置
    int indexEnd = str.indexOf("};var alarmDZ");

    String jsonCityDZ = str.substring(indexStart + 13, indexEnd); // 复制字符串
    Serial.println(jsonCityDZ);

    indexStart = str.indexOf("dataSK ="); // 寻找起始和结束位置
    indexEnd = str.indexOf(";var dataZS");
    String jsonDataSK = str.substring(indexStart + 8, indexEnd); // 复制字符串
    Serial.println(jsonDataSK);

    indexStart = str.indexOf("\"f\":["); // 寻找起始和结束位置
    indexEnd = str.indexOf(",{\"fa");
    String jsonFC = str.substring(indexStart + 5, indexEnd); // 复制字符串
    Serial.println(jsonFC);

    weaterData(&jsonCityDZ, &jsonDataSK, &jsonFC); // 显示天气信息
    Serial.println("获取成功");
  }
  else
  {
    Serial.println("请求城市天气错误：");
    Serial.print(httpCode);
  }

  httpClient.end(); // 关闭与服务器连接
}

void weaterData(String *cityDZ, String *dataSK, String *dataFC) // 天气信息写到屏幕上
{
  DynamicJsonDocument doc(512);
  deserializeJson(doc, *dataSK);
  JsonObject sk = doc.as<JsonObject>();
  String temp_str = sk["temp"].as<String>() + "℃";
  String hum_str = sk["SD"].as<String>();

  clk.setColorDepth(8);
  clk.loadFont(ZdyLwFont_20); // 加载汉字字体

  // 温度显示
  clk.createSprite(54, 32);              // 创建Sprite
  clk.fillSprite(bgColor);               // 填充颜色
  clk.setTextDatum(CC_DATUM);            // 显示对齐方式
  clk.setTextColor(frontColor, bgColor); // 文本的前景色和背景色
  clk.drawString(temp_str, 27, 16); // 显示文本
  clk.pushSprite(185, 168); // Sprite中内容一次推向屏幕
  clk.deleteSprite();       // 删除Sprite

  // 城市名称显示
  clk.createSprite(88, 32);
  clk.fillSprite(bgColor);
  clk.setTextDatum(CC_DATUM);
  clk.setTextColor(frontColor, bgColor);
  clk.drawString(sk["cityname"].as<String>(), 44, 16);
  clk.pushSprite(151, 1);
  clk.deleteSprite();

  // PM2.5空气指数显示
  uint16_t pm25BgColor = tft.color565(156, 202, 127); // 优
  String aqiTxt = "优";
  int pm25V = sk["aqi"];
  if (pm25V > 200)
  {
    pm25BgColor = tft.color565(136, 11, 32); // 重度，显示颜色和空气质量程度
    aqiTxt = "重度";
  }
  else if (pm25V > 150)
  {
    pm25BgColor = tft.color565(186, 55, 121); // 中度
    aqiTxt = "中度";
  }
  else if (pm25V > 100)
  {
    pm25BgColor = tft.color565(242, 159, 57); // 轻
    aqiTxt = "轻度";
  }
  else if (pm25V > 50)
  {
    pm25BgColor = tft.color565(247, 219, 100); // 良
    aqiTxt = "良";
  }
  clk.createSprite(50, 24);
  clk.fillSprite(bgColor);
  clk.fillRoundRect(0, 0, 50, 24, 4, pm25BgColor);
  clk.setTextDatum(CC_DATUM);
  clk.setTextColor(0xFFFF);
  clk.drawString(aqiTxt, 25, 13);
  clk.pushSprite(5, 130);
  clk.deleteSprite();

  // 湿度显示
  clk.createSprite(56, 24);
  clk.fillSprite(bgColor);
  clk.setTextDatum(CC_DATUM);
  clk.setTextColor(frontColor, bgColor);
  clk.drawString(hum_str, 28, 13);

  // clk.drawString("100%",28,13);
  clk.pushSprite(180, 130);
  clk.deleteSprite();

  scrollText[0] = "实时天气 " + sk["weather"].as<String>(); // 滚动显示的数据缓冲区
  scrollText[1] = "空气质量 " + aqiTxt;
  scrollText[2] = "风向 " + sk["WD"].as<String>() + sk["WS"].as<String>();

  // 左上角滚动字幕
  deserializeJson(doc, *cityDZ);
  JsonObject dz = doc.as<JsonObject>();
  Serial.println(sk["ws"].as<String>());

  scrollText[3] = "今日" + dz["weather"].as<String>();

  deserializeJson(doc, *dataFC);
  JsonObject fc = doc.as<JsonObject>();
  scrollText[4] = "最低温度" + fc["fd"].as<String>() + "℃";
  scrollText[5] = "最高温度" + fc["fc"].as<String>() + "℃";

  scrollText[6] = "室外温度" + temp_str;
  scrollText[7] = "室外湿度" + hum_str;

  clk.unloadFont(); // 卸载字体
}

void scrollBanner() // 天气滚动条显示
{
  unsigned long now1 = millis();
  if (now1 - LastTime1 > 2500)
  {
    // 2.5秒切换一次显示内容
    if (scrollText[Dis_Count])
    { // 如果滚动显示缓冲区有数据
      clkb.setColorDepth(8);
      clkb.loadFont(ZdyLwFont_20); // 加载汉字字体
      // 循环，7循环数，是因为一次Dis_Scroll调用大概占用7ms，7次调用大概等于一个动画显示的延时50ms
      // 如果一个动画帧延时100，则要设置 100/7 约为14或15
      for (int a = 0; a < 7; pos--, a++)
      { // 24点，每次移动一个点，从下往上移
        if (pos > 0)
          Dis_Scroll(pos);
        else
        {
          pos = 24;
          break;
        }
      }
      if (pos > 0 && pos != 24) // 大于0，说明没有循环完，但是要退出显示动画图片，回来还要继续移动
      {
        // clkb.deleteSprite();                      //删除Sprite，这个我移动到Dis_Scroll函数里了
        clkb.unloadFont(); // 卸载字体
        return;
      }
      clkb.unloadFont(); // 卸载字体
      if (Dis_Count >= 7)
      {                // 总共显示 8 条信息 (index 0-7)
        Dis_Count = 0; // 回第一个
      }
      else
      {
        Dis_Count += 1; // 准备切换到下一个
      }
      Serial.println(Dis_Count);
    }
    LastTime1 = now1;
  }
}

void Dis_Scroll(int pos)
{                             // 字体滚动
  clkb.createSprite(148, 24); // 创建Sprite，先在Sprite内存中画点，然后将内存中的点一次推向屏幕，这样刷新比较快
  clkb.fillSprite(bgColor);   // 背景色
  clkb.setTextWrap(false);
  clkb.setTextDatum(CC_DATUM);                          // 显示对齐方式
  clkb.setTextColor(frontColor, bgColor);               // 文本的前景色和背景色
  clkb.drawString(scrollText[Dis_Count], 74, pos + 12); // 打显示内容
  clkb.pushSprite(2, 4);                                // Sprite中内容一次推向屏幕
  clkb.deleteSprite();                                  // 删除Sprite
}

void imgAnim()
{
  int x = 80, y = 94, dt = 30; // 瘦子版dt=10毫秒 胖子30较为合适

  TJpgDec.drawJpg(x, y, i0, sizeof(i0)); // 打一张图片延时一段时间，达到动画效果
  delay(dt);
  TJpgDec.drawJpg(x, y, i1, sizeof(i1));
  delay(dt);
  TJpgDec.drawJpg(x, y, i2, sizeof(i2));
  delay(dt);
  TJpgDec.drawJpg(x, y, i3, sizeof(i3));
  delay(dt);
  TJpgDec.drawJpg(x, y, i4, sizeof(i4));
  delay(dt);
  TJpgDec.drawJpg(x, y, i5, sizeof(i5));
  delay(dt);
  TJpgDec.drawJpg(x, y, i6, sizeof(i6));
  delay(dt);
  TJpgDec.drawJpg(x, y, i7, sizeof(i7));
  delay(dt);
  TJpgDec.drawJpg(x, y, i8, sizeof(i8));
  delay(dt);
  TJpgDec.drawJpg(x, y, i9, sizeof(i9));
  delay(dt);
}
unsigned long oldTime = 0, imgNum = 1;
void imgDisplay()
{
  int x = 75, y = 94, dt;
  switch (Gif_Mode)
  { // 修改动画的播放速度
  case 1:  dt = 100; break;
  case 2:  dt = 50;  break;
  case 3:  dt = 100; break;
  case 4:  dt = 100; break;
  case 5:  dt = 50;  break;
  }
  if (millis() - oldTime >= dt)
  {
    imgNum++;
    oldTime = millis();
  }
  else
    return;

  // === 动画-跑步的老头 (12帧) ===
  if (Gif_Mode == 5)
  {
    const uint8_t *frames[] = {my_1, my_2, my_3, my_4, my_5, my_6, my_7, my_8, my_9, my_10, my_11, my_12};
    const size_t sizes[] = {sizeof(my_1), sizeof(my_2), sizeof(my_3), sizeof(my_4), sizeof(my_5), sizeof(my_6), sizeof(my_7), sizeof(my_8), sizeof(my_9), sizeof(my_10), sizeof(my_11), sizeof(my_12)};
    const int maxFrame = 12;
    TJpgDec.drawJpg(x, y, frames[imgNum - 1], sizes[imgNum - 1]);
    if (imgNum >= maxFrame) imgNum = 1;
  }
  // === 动画-龙猫转圈 (80帧) ===
  else if (Gif_Mode == 3)
  {
    const uint8_t *frames[] = {img_0, img_1, img_2, img_3, img_4, img_5, img_6, img_7, img_8, img_9, img_10, img_11, img_12, img_13, img_14, img_15, img_16, img_17, img_18, img_19, img_20, img_21, img_22, img_23, img_24, img_25, img_26, img_27, img_28, img_29, img_30, img_31, img_32, img_33, img_34, img_35, img_36, img_37, img_38, img_39, img_40, img_41, img_42, img_43, img_44, img_45, img_46, img_47, img_48, img_49, img_50, img_51, img_52, img_53, img_54, img_55, img_56, img_57, img_58, img_59, img_60, img_61, img_62, img_63, img_64, img_65, img_66, img_67, img_68, img_69, img_70, img_71, img_72, img_73, img_74, img_75, img_76, img_77, img_78, img_79};
    const size_t sizes[] = {sizeof(img_0), sizeof(img_1), sizeof(img_2), sizeof(img_3), sizeof(img_4), sizeof(img_5), sizeof(img_6), sizeof(img_7), sizeof(img_8), sizeof(img_9), sizeof(img_10), sizeof(img_11), sizeof(img_12), sizeof(img_13), sizeof(img_14), sizeof(img_15), sizeof(img_16), sizeof(img_17), sizeof(img_18), sizeof(img_19), sizeof(img_20), sizeof(img_21), sizeof(img_22), sizeof(img_23), sizeof(img_24), sizeof(img_25), sizeof(img_26), sizeof(img_27), sizeof(img_28), sizeof(img_29), sizeof(img_30), sizeof(img_31), sizeof(img_32), sizeof(img_33), sizeof(img_34), sizeof(img_35), sizeof(img_36), sizeof(img_37), sizeof(img_38), sizeof(img_39), sizeof(img_40), sizeof(img_41), sizeof(img_42), sizeof(img_43), sizeof(img_44), sizeof(img_45), sizeof(img_46), sizeof(img_47), sizeof(img_48), sizeof(img_49), sizeof(img_50), sizeof(img_51), sizeof(img_52), sizeof(img_53), sizeof(img_54), sizeof(img_55), sizeof(img_56), sizeof(img_57), sizeof(img_58), sizeof(img_59), sizeof(img_60), sizeof(img_61), sizeof(img_62), sizeof(img_63), sizeof(img_64), sizeof(img_65), sizeof(img_66), sizeof(img_67), sizeof(img_68), sizeof(img_69), sizeof(img_70), sizeof(img_71), sizeof(img_72), sizeof(img_73), sizeof(img_74), sizeof(img_75), sizeof(img_76), sizeof(img_77), sizeof(img_78), sizeof(img_79)};
    const int maxFrame = 80;
    TJpgDec.drawJpg(x, y, frames[imgNum - 1], sizes[imgNum - 1]);
    if (imgNum >= maxFrame) imgNum = 1;
  }
  // === 动画-打乒乓 (27帧, 不含11号) ===
  else if (Gif_Mode == 1)
  {
    const uint8_t *frames[] = {pingpang_0, pingpang_1, pingpang_2, pingpang_3, pingpang_4, pingpang_5, pingpang_6, pingpang_7, pingpang_8, pingpang_9, pingpang_10, pingpang_12, pingpang_13, pingpang_14, pingpang_15, pingpang_16, pingpang_17, pingpang_18, pingpang_19, pingpang_20, pingpang_21, pingpang_22, pingpang_23, pingpang_24, pingpang_25, pingpang_26, pingpang_27};
    const size_t sizes[] = {sizeof(pingpang_0), sizeof(pingpang_1), sizeof(pingpang_2), sizeof(pingpang_3), sizeof(pingpang_4), sizeof(pingpang_5), sizeof(pingpang_6), sizeof(pingpang_7), sizeof(pingpang_8), sizeof(pingpang_9), sizeof(pingpang_10), sizeof(pingpang_12), sizeof(pingpang_13), sizeof(pingpang_14), sizeof(pingpang_15), sizeof(pingpang_16), sizeof(pingpang_17), sizeof(pingpang_18), sizeof(pingpang_19), sizeof(pingpang_20), sizeof(pingpang_21), sizeof(pingpang_22), sizeof(pingpang_23), sizeof(pingpang_24), sizeof(pingpang_25), sizeof(pingpang_26), sizeof(pingpang_27)};
    const int maxFrame = 27;
    TJpgDec.drawJpg(x, y, frames[imgNum - 1], sizes[imgNum - 1]);
    if (imgNum >= maxFrame) imgNum = 1;
  }
  // === 动画-太空人 (10帧) ===
  else if (Gif_Mode == 4)
  {
    const uint8_t *frames[] = {i0, i1, i2, i3, i4, i5, i6, i7, i8, i9};
    const size_t sizes[] = {sizeof(i0), sizeof(i1), sizeof(i2), sizeof(i3), sizeof(i4), sizeof(i5), sizeof(i6), sizeof(i7), sizeof(i8), sizeof(i9)};
    const int maxFrame = 10;
    TJpgDec.drawJpg(x, y, frames[imgNum - 1], sizes[imgNum - 1]);
    if (imgNum >= maxFrame) imgNum = 1;
  }
  // === 动画-龙猫跳绳 (40帧, y=84) ===
  else if (Gif_Mode == 2)
  {
    const uint8_t *frames[] = {quan_0, quan_1, quan_2, quan_3, quan_4, quan_5, quan_6, quan_7, quan_8, quan_9, quan_10, quan_11, quan_12, quan_13, quan_14, quan_15, quan_16, quan_17, quan_18, quan_19, quan_20, quan_21, quan_22, quan_23, quan_24, quan_25, quan_26, quan_27, quan_28, quan_29, quan_30, quan_31, quan_32, quan_33, quan_34, quan_35, quan_36, quan_37, quan_38, quan_39};
    const size_t sizes[] = {sizeof(quan_0), sizeof(quan_1), sizeof(quan_2), sizeof(quan_3), sizeof(quan_4), sizeof(quan_5), sizeof(quan_6), sizeof(quan_7), sizeof(quan_8), sizeof(quan_9), sizeof(quan_10), sizeof(quan_11), sizeof(quan_12), sizeof(quan_13), sizeof(quan_14), sizeof(quan_15), sizeof(quan_16), sizeof(quan_17), sizeof(quan_18), sizeof(quan_19), sizeof(quan_20), sizeof(quan_21), sizeof(quan_22), sizeof(quan_23), sizeof(quan_24), sizeof(quan_25), sizeof(quan_26), sizeof(quan_27), sizeof(quan_28), sizeof(quan_29), sizeof(quan_30), sizeof(quan_31), sizeof(quan_32), sizeof(quan_33), sizeof(quan_34), sizeof(quan_35), sizeof(quan_36), sizeof(quan_37), sizeof(quan_38), sizeof(quan_39)};
    const int maxFrame = 40;
    TJpgDec.drawJpg(x, 84, frames[imgNum - 1], sizes[imgNum - 1]); // y=84
    if (imgNum >= maxFrame) imgNum = 1;
  }
}

void PowerOn_Loading(uint8_t delayTime) // 开机联网显示的进度条，输入延时时间
{
  clk.setColorDepth(8);
  clk.createSprite(200, 50); // 创建Sprite
  clk.fillSprite(0x0000);    // 填充颜色

  clk.drawRoundRect(0, 0, 200, 16, 8, 0xFFFF);     // 画一个圆角矩形
  clk.fillRoundRect(3, 3, loadNum, 10, 5, 0xFFFF); // 画一个填充的圆角矩形
  clk.setTextDatum(CC_DATUM);                      // 显示对齐方式
  clk.setTextColor(TFT_GREEN, 0x0000);             // 文本的前景色和背景色
  if (SmartConfigStatus == 1)
    clk.drawString("Waiting for Config", 100, 40, 2);
  else if (SmartConfigStatus == 2)
    clk.drawString("Connecting to WiFi", 100, 40, 2);
  else if (SmartConfigStatus == 4)
    clk.drawString("WiFi Connected.", 100, 40, 2);
  else
    clk.drawString("Connecting to WiFi", 100, 40, 2);                    // 显示文本
  clk.pushSprite(20, 110);                                               // Sprite中内容一次推向屏幕
  clk.deleteSprite();                                                    // 删除Sprite
  if (wifiConnected == false && loadNum > 160 && SmartConfigStatus == 0) // wifi没有连接时，进度条不再增长
  {
  }
  else if (SmartConfigStatus != 0 && SmartConfigStatus != 4 && loadNum > 180) // 在smart config 状态时，进度条反复回退
  {
    loadNum = 161;
  }
  else
  {
    loadNum += 1;
  }
  // 进度条位置变化，直到加载完成
  if (loadNum >= 194)
  {
    loadNum = 194;
  }
  delay(delayTime);
}

// 自动配网
bool smart_config()
{
  uint8_t cnt = 1;

  Serial.println("\n========== SmartConfig 开始 ==========");
  Serial.print("WiFi 模式: ");
  if (WiFi.getMode() & WIFI_STA) Serial.print("STA ");
  if (WiFi.getMode() & WIFI_AP) Serial.print("AP ");
  Serial.print("(固件设置: WIFI_STA)");
  Serial.println();
  Serial.print("WiFi 芯片 MAC: ");
  Serial.println(WiFi.macAddress());

  WiFi.mode(WIFI_STA);
  WiFi.beginSmartConfig();
  Serial.println("\n[SmartConfig] 等待手机 App 发送配网信息...");
  Serial.println("[SmartConfig] 请使用 ESP8266 SmartConfig App 或 安信可小程序");
  Serial.println("[SmartConfig] 确保手机已连接 2.4G WiFi (不支持 5G)");
  SmartConfigStatus = 1;

  unsigned long lastStatusTime = millis();
  while (!WiFi.smartConfigDone())
  {
    for (uint8_t n = 0; n < 10; n++)
    { // 每500毫秒检测一次状态
      PowerOn_Loading(50);
    }
    Serial.print(".");

    // 每 10 秒输出一次状态摘要
    if (millis() - lastStatusTime > 10000)
    {
      Serial.println();
      Serial.print("[SmartConfig] 仍在等待... 已等待 ");
      Serial.print((millis() - lastStatusTime) / 1000);
      Serial.println(" 秒");
      Serial.print("[SmartConfig] WiFi 状态: ");
      switch (WiFi.status())
      {
        case WL_NO_SSID_AVAIL: Serial.println("无可用 SSID"); break;
        case WL_CONNECTED: Serial.println("已连接"); break;
        case WL_DISCONNECTED: Serial.println("未连接"); break;
        default: Serial.println(WiFi.status()); break;
      }
      Serial.print("[SmartConfig] smartConfigDone(): ");
      Serial.println(WiFi.smartConfigDone() ? "true" : "false");
      lastStatusTime = millis();
    }
  }

  Serial.println("");
  Serial.println("\n[SmartConfig] ✓ 收到配网信息!");
  Serial.print("[SmartConfig] SSID: ");
  Serial.println(WiFi.SSID());
  Serial.print("[SmartConfig] 信道: ");
  Serial.println(WiFi.channel());
  SmartConfigStatus = 2;

  // Wait for WiFi to connect to AP
  Serial.println("\n[SmartConfig] 正在连接 WiFi...");
  lastStatusTime = millis();
  while (WiFi.status() != WL_CONNECTED)
  {
    for (uint8_t n = 0; n < 10; n++)
    { // 每500毫秒检测一次状态
      PowerOn_Loading(50);
    }
    cnt = cnt + 1;

    // 每 5 秒输出连接状态
    if (millis() - lastStatusTime > 5000)
    {
      Serial.println();
      Serial.print("[SmartConfig] 连接状态: ");
      int wlStatus = WiFi.status();
      switch (wlStatus)
      {
        case WL_IDLE_STATUS:     Serial.println("空闲 (WL_IDLE_STATUS)"); break;
        case WL_NO_SSID_AVAIL:   Serial.println("SSID 不可用 (WL_NO_SSID_AVAIL)"); break;
        case WL_SCAN_COMPLETED:  Serial.println("扫描完成 (WL_SCAN_COMPLETED)"); break;
        case WL_CONNECTED:       Serial.println("已连接 (WL_CONNECTED)"); break;
        case WL_CONNECT_FAILED:  Serial.println("连接失败 (WL_CONNECT_FAILED)"); break;
        case WL_CONNECTION_LOST: Serial.println("连接丢失 (WL_CONNECTION_LOST)"); break;
        case WL_DISCONNECTED:    Serial.println("断开 (WL_DISCONNECTED)"); break;
        default:                 Serial.printf("未知 (%d)\n", wlStatus); break;
      }
      lastStatusTime = millis();
    }

    if (cnt > wifi_connect_cnt / 2)
    {
      Serial.println("\n[SmartConfig] ✗ WiFi 连接超时!");
      Serial.println("[SmartConfig]   可能原因:");
      Serial.println("[SmartConfig]   1. WiFi 密码错误");
      Serial.println("[SmartConfig]   2. 路由器不在 2.4G 频段");
      Serial.println("[SmartConfig]   3. 信号太弱");
      Serial.print("[SmartConfig]   SSID: ");
      Serial.println(WiFi.SSID());
      SmartConfigStatus = 3;
      WiFi.stopSmartConfig(); // 停止smartconfig，为下一轮配置准备
      return false;
    }
  }

  Serial.println("");
  Serial.println("\n[SmartConfig] ✓ WiFi 连接成功!");
  Serial.print("[SmartConfig] IP 地址: ");
  Serial.println(WiFi.localIP());
  Serial.print("[SmartConfig] RSSI: ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");
  SmartConfigStatus = 4;
  local_IP = WiFi.localIP().toString();

  delay(5);
  strcpy(wifiConf.wifi_ssid, WiFi.SSID().c_str());
  strcpy(wifiConf.wifi_password, WiFi.psk().c_str());
  writeWifiConf();
  Serial.println("[SmartConfig] ✓ SSID/密码已保存到 EEPROM");
  Serial.println("========== SmartConfig 结束 ==========\n");
  return true;
}
// 切换背景色，切换字体颜色
void change_color()
{
  // 全局统一：黑底白字
  frontColor = TFT_WHITE;
  bgColor = TFT_BLACK;

  // 绘制一个窗口
  tft.setViewport(0, 20, 240, 202);              // 中间的显示区域大小
  tft.fillScreen(0x0000);                        // 清屏
  tft.fillRoundRect(0, 0, 240, 202, 5, bgColor); // 实心圆角矩形
  // tft.resetViewport();

  // 绘制线框
  tft.drawFastHLine(0, 34, 240, frontColor); // 这些坐标都是窗体内部坐标
  tft.drawFastVLine(150, 0, 34, frontColor);
  tft.drawFastHLine(0, 166, 240, frontColor);
  tft.drawFastVLine(60, 166, 34, frontColor);
  tft.drawFastVLine(160, 166, 34, frontColor);
  tft.drawFastHLine(0, 202, 240, frontColor);

  getCityCode(); // 通过IP地址获取城市代码

  TJpgDec.drawJpg(161, 171, temperature, sizeof(temperature)); // 温度图标
  TJpgDec.drawJpg(159, 130, humidity, sizeof(humidity));       // 湿度图标

  digitalClockDisplay();
}
// mqtt 收到订阅消息后的回调函数
void mqtt_callback(char *topic, byte *payload, unsigned int length)
{
  Serial.print("Message arrived in topic: ");
  Serial.println(topic);
  Serial.print("Message:");
  Serial.printf("%s\n", (char *)payload);
  if (0 == strcmp(topic, MQTT_TOPIC_PIC))
  {
    // 直接返回一个数值，用来控制显示的图片
    if (length == 1 && ((int)payload[0] - 48) > 0 && ((int)payload[0] - 48) < 6)
    {
      int old_mode = Gif_Mode;
      Gif_Mode = (int)payload[0] - 48;
      imgNum = 1;
      if ((old_mode == 5 && Gif_Mode < 5) || (old_mode < 5 && Gif_Mode == 5))
      {
        // 切换背景色，切换字体颜色
        change_color();
        // 保存 Gif_Mode 到eeprom
        wifiConf.gif_mode = Gif_Mode;
        writeWifiConf();
      }
    }
    Serial.printf("length:%d,Gif_Mode:%d\n", length, Gif_Mode);
  }
  else if (0 == strcmp(topic, MQTT_TOPIC_LED))
  {
    char message[64];
    uint8_t msgLen = min(length, sizeof(message) - 1);
    memcpy(message, payload, msgLen);
    message[msgLen] = '\0';
    Serial.print(message);
    if (strcmp(message, "on") == 0)
      digitalWrite(LED, LOW);   // LED on (低电平)
    else if (strcmp(message, "off") == 0)
      digitalWrite(LED, HIGH);  // LED off (高电平)
    Serial.println();
    Serial.println("-----------------------");
  }

}

void setUpOverTheAirProgramming() // OAT升级
{

  // Change OTA port.
  // Default: 8266
  // ArduinoOTA.setPort(8266);

  // Change the name of how it is going to
  // show up in Arduino IDE.
  // Default: esp8266-[ChipID]
  // ArduinoOTA.setHostname("myesp8266");

  // Re-programming passowrd.
  // No password by default.
  // ArduinoOTA.setPassword("123");

  ArduinoOTA.begin();
}

/* 3. 处理访问网站根目录“/”的访问请求 */
void handleRoot()
{
  // 565 转rgb
  int color_red = ((frontColor >> 11) & 0xff) << 3;
  int color_green = ((frontColor >> 5) & 0x3f) << 2;
  int color_blue = (frontColor & 0x1f) << 3;
  String htmlCode = "<!DOCTYPE html>\n";
  htmlCode += " <html>\n";
  htmlCode += "   <head>\n";
  htmlCode += "     <meta charset=\"UTF-8\"/>\n";
  htmlCode += "     <title>ESP8266控制</title>\n";
  htmlCode += "   </head>\n";
  htmlCode += "   <body>\n<div style=\"width:600px;margin:0 auto;\">\n";

  htmlCode += "     <h2 align=\"center\">esp8266显示屏参数控制</h2>";
  htmlCode += "     <p>\n<form action=\"/gifmode\" method=\"POST\">\n";
  htmlCode += "       <a>设置动图样式：</a>\n";
  htmlCode += "     	<select name=\"gifmode\">\n";
  if (Gif_Mode == 1)
    htmlCode += "   		<option value=\"1\" selected>乒乓球</option>\n";
  else
    htmlCode += "   		<option value=\"1\">乒乓球</option>\n";
  if (Gif_Mode == 2)
    htmlCode += "   		<option value=\"2\" selected>跳绳的龙猫</option>\n";
  else
    htmlCode += "   		<option value=\"2\">跳绳的龙猫</option>\n";
  if (Gif_Mode == 3)
    htmlCode += "   		<option value=\"3\" selected>跳舞的龙猫</option>\n";
  else
    htmlCode += "   		<option value=\"3\">跳舞的龙猫</option>\n";
  if (Gif_Mode == 4)
    htmlCode += "   		<option value=\"4\" selected>太空人</option>\n";
  else
    htmlCode += "   		<option value=\"4\">太空人</option>\n";
  if (Gif_Mode == 5)
    htmlCode += "   		<option value=\"5\" selected>跑步的老头</option>\n";
  else
    htmlCode += "   		<option value=\"5\">跑步的老头</option>\n";
  htmlCode += "     	</select>\n";
  htmlCode += "     	<input type=\"submit\" value=\"提交\" />\n";
  htmlCode += "     </form>\n</p>\n";
  htmlCode += "     <h2 align=\"center\">设置天气网城市代码</h2>";
  htmlCode += "     <p>\n<form action=\"/city\" method=\"POST\">\n";
  htmlCode += "       <p><a>城市代码(9位数字)</a>\n";
  htmlCode += "     	<input  name=\"citycode\" value=\"" + String(wifiConf.city_id) + "\" />\n</p>\n";
  htmlCode += "     	<input type=\"submit\" value=\"设置\" />\n";
  htmlCode += "     </form>\n</p>\n";
  htmlCode += "     <h2 align=\"center\">设置字体颜色(RGB)</h2>";
  htmlCode += "     <p>\n<form action=\"/color\" method=\"POST\">\n";
  htmlCode += "       <p><a>字体颜色RED(0-255)：</a>\n";
  htmlCode += "     	<input  name=\"red\" value=\"" + String(color_red) + "\" />\n</p>\n";
  htmlCode += "       <p><a>字体颜色GREEN(0-255)：</a>\n";
  htmlCode += "     	<input  name=\"green\" value=\"" + String(color_green) + "\" />\n</p>\n";
  htmlCode += "       <p><a>字体颜色BLUE(0-255)：</a>\n";
  htmlCode += "     	<input  name=\"blue\" value=\"" + String(color_blue) + "\" />\n</p>\n";
  htmlCode += "     	<input type=\"submit\" value=\"设置\" />\n";
  htmlCode += "     </form>\n</p>\n";
  htmlCode += "     <h2 align=\"center\">重启设备</h2>";
  htmlCode += "     <p>\n<form action=\"/restart\" method=\"POST\">\n";
  htmlCode += "       <a>点击按钮重启设备：</a>\n";
  htmlCode += "     	<input type=\"hidden\" name=\"restart\" value=\"yes\" />\n";
  htmlCode += "     	<input type=\"submit\" value=\"确认重启\" />\n";
  htmlCode += "     </form>\n</p>\n";
  htmlCode += "     </div>\n";
  htmlCode += "   </body>\n";
  htmlCode += "</html>\n";
  esp8266_server.send(200, "text/html", htmlCode); // NodeMCU将调用此函数。
}

/* 4. 设置处理404情况的函数'handleNotFound' */
void handleNotFound()
{                                                           // 当浏览器请求的网络资源无法在服务器找到时，
  esp8266_server.send(404, "text/plain", "404: Not found"); // NodeMCU将调用此函数。
}

/*HTTP server设置 图片样式*/
void handle_Gif_Mode()
{
  if (esp8266_server.hasArg("gifmode"))
  {
    int value = 0;
    // esp8266_server.arg("gifmode").toCharArray(value, 1);
    value = (int)esp8266_server.arg("gifmode").toInt();
    Serial.printf("http server提交的gifmode 参数为%d\n", value);

    // 直接返回一个数值，用来控制显示的图片
    if (value > 0 && value < 6)
    {
      int old_mode = Gif_Mode;
      Gif_Mode = value;
      imgNum = 1;
      if ((old_mode == 5 && Gif_Mode < 5) || (old_mode < 5 && Gif_Mode == 5))
      {
        // 切换背景色，切换字体颜色
        change_color();
        // 保存 Gif_Mode 到eeprom
        wifiConf.gif_mode = Gif_Mode;
        writeWifiConf();
      }
    }
  }
  esp8266_server.sendHeader("Location", "/", true); // Redirect to our html web page
  esp8266_server.send(302, "text/plane", "");
}

// http server 设置字体颜色响应
void handle_color()
{
  esp8266_server.sendHeader("Location", "/", true); // Redirect to our html web page
  esp8266_server.send(302, "text/plane", "");
  if (esp8266_server.hasArg("red") && esp8266_server.hasArg("green") && esp8266_server.hasArg("blue"))
  {
    int red = 0, green = 0, blue = 0;
    red = esp8266_server.arg("red").toInt();
    green = esp8266_server.arg("green").toInt();
    blue = esp8266_server.arg("blue").toInt();

    if (red < 256 && red >= 0 && green < 256 && green >= 0 && blue < 256 && blue >= 0) // 判断参数
    {
      // rgb 转565色值
      frontColor = (((red & 0xf8) >> 3) << 11) + (((green & 0xfc) >> 2) << 5) + ((blue & 0xf8) >> 3);
      wifiConf.frontColor = frontColor;
      writeWifiConf();
      // getCityWeater();
      change_color();
    }
  }
}

void handle_citycode()
{
  esp8266_server.sendHeader("Location", "/", true); // Redirect to our html web page
  esp8266_server.send(302, "text/plane", "");
  if (esp8266_server.hasArg("citycode"))
  {
    String citycode = esp8266_server.arg("citycode");
    if (citycode.startsWith("101") && citycode.length() == 9)
    {
      strcpy(wifiConf.city_id, citycode.c_str());
      writeWifiConf();
      cityCode = citycode;
      getCityCode();
    }
  }
}

// http server重启响应
void handle_restart()
{
  esp8266_server.sendHeader("Location", "/", true); // Redirect to our html web page
  esp8266_server.send(302, "text/plane", "");
  if (esp8266_server.hasArg("restart"))
  {
    int restart = 2;
    restart = esp8266_server.arg("restart").compareTo("yes");
    if (restart == 0)
    {
      delay(1000); // 等待1s让浏览器得到返回结果
      ESP.restart();
    }
  }
}





void setup()
{
  Serial.begin(115200); // 初始化串口
  Serial.println();     // 打印回车换行

  EEPROM.begin(512); // 读取eeprom配置
  readWifiConf();

  if (wifiConf.frontColor != 0 && wifiConf.frontColor != 0xFFFF)
    frontColor = wifiConf.frontColor;
  else
    frontColor = TFT_WHITE;

  tft.init();                            // TFT初始化
  tft.setRotation(0);                    // 旋转角度0-3
  tft.fillScreen(0x0000);                // 清屏
  tft.setTextColor(frontColor, bgColor); // 设置字体颜色

  // === LCD 32x32 块测试（纯测试，不进时钟）===
  lcdBlockTest();
  Serial.println("LCD 32x32 块测试完成，停留在测试画面");
  while (1) { delay(1000); }

  connect_wifi(); // 联网处理

  Gif_Mode = wifiConf.gif_mode;

  // 连接mqtt服务器
  mqtt_client.setServer(mqtt_broker, mqtt_port);
  mqtt_client.setCallback(mqtt_callback);
  while (!mqtt_client.connected())
  {
    String client_id = "esp8266-client-";
    client_id += String(WiFi.macAddress());
    Serial.printf("The client %s connects to the public mqtt broker\n", client_id.c_str());
    if (mqtt_client.connect(client_id.c_str(), mqtt_username, mqtt_password))
    {
      Serial.println("Public emqx mqtt broker connected");
    }
    else
    {
      Serial.print("failed with state ");
      Serial.print(mqtt_client.state());
      delay(2000);
    }
  }
  mqtt_client.subscribe(topic);

  setUpOverTheAirProgramming(); // 开启OTA升级服务

  Serial.println("Starting UDP"); // 连接时间服务器
  Udp.begin(localPort);
  Serial.print("Local port: ");
  Serial.println(Udp.localPort());
  Serial.println("waiting for sync");
  setSyncProvider(getNtpTime);
  setSyncInterval(300);

  TJpgDec.setJpgScale(1);          // 设置放大倍数
  TJpgDec.setSwapBytes(true);      // 交换字节
  TJpgDec.setCallback(tft_output); // 回调函数

  TJpgDec.drawJpg(0, 0, watchtop, sizeof(watchtop)); // 显示顶部图标 240*20
  // TJpgDec.drawJpg(0, 220, watchbottom, sizeof(watchbottom)); // 显示底部图标 240*20

  // 底部显示ip的区域
  // clk.loadFont(ZdyLwFont_20); // 加载汉字字体
  tft.setViewport(0, 222, 240, 18);
  tft.fillScreen(TFT_BLACK); // 黑色背景
  // IP显示
  clk.createSprite(240, 18); // 创建Sprite
  // clk.fillSprite(frontColor);               // 填充颜色
  clk.setTextDatum(CL_DATUM);                 // 显示对齐方式
  clk.setTextColor(TFT_WHITE, TFT_BLACK);     // 文本的前景色和背景色
  clk.drawString("IP:" + local_IP, 5, 10, 2); // 显示文本
  clk.pushSprite(0, 0);                       // Sprite中内容一次推向屏幕
  clk.deleteSprite();                         // 删除Sprite
  tft.resetViewport();

  // clk.unloadFont(); // 卸载字体



  change_color();

  httpUpdater.setup(&esp8266_server);
  /* 3. 开启http网络服务器功能 */
  esp8266_server.begin();                    // 启动http网络服务器
  esp8266_server.on("/", handleRoot);        // 设置请求根目录时的处理函数函数
  esp8266_server.onNotFound(handleNotFound); // 设置无法响应时的处理函数

  esp8266_server.on("/gifmode", handle_Gif_Mode); // 处理动图类型的url响应函数
  esp8266_server.on("/restart", handle_restart);  // 处理软件复位的url响应函数
  esp8266_server.on("/color", handle_color);      // 处理设置字体颜色的url响应函数
  esp8266_server.on("/city", handle_citycode);    // 处理设置城市代码响应函数
}

void loop()
{

  if (timeStatus() != timeNotSet)
  { // 已经获取到数据的话
    if (now() != prevDisplay)
    { // 如果本次数据和上次不一样的话，刷新
      prevDisplay = now();
      digitalClockDisplay();
    }
  }


  if (millis() - LastTime2 > 600000)
  { // 10分钟更新一次天气
    LastTime2 = millis();
    getCityWeater();
  }
  scrollBanner(); // 天气数据滚动显示 //该函数执行时，会使imgDisplay的动画卡顿一下
  imgDisplay();   // 龙猫动画

  ArduinoOTA.handle(); // OTA升级



  // mqtt
  mqtt_client.loop();
  // http server
  esp8266_server.handleClient();
}

// ============================================================
// LCD 块颜色测试 — 图片缓存区方案（Sprite）
// FPS 只输出到串口，屏幕不显示任何文字
// ============================================================
void lcdBlockTest()
{
  const int BLOCK_SIZE = 40;
  const int COLS = 240 / BLOCK_SIZE;  // 6 列
  const int ROWS = 240 / BLOCK_SIZE;  // 6 行
  const int TOTAL_BLOCKS = COLS * ROWS;

  // 先清屏为黑色
  tft.fillScreen(TFT_BLACK);

  // === 创建图片缓存区（Sprite）===
  // 分配 BLOCK_SIZE * BLOCK_SIZE * 2 字节的 RAM
  TFT_eSprite block(&tft);
  block.createSprite(BLOCK_SIZE, BLOCK_SIZE);

  // 初始化随机种子
  randomSeed(micros());

  // 帧率计数变量
  unsigned long frameCount = 0;
  unsigned long lastSerialTime = millis();

  while (1)  // 持续循环，画面不断变动
  {
    // 记录单帧开始时间
    unsigned long frameStart = micros();

    for (int idx = 0; idx < TOTAL_BLOCKS; idx++)
    {
      int row = idx / COLS;
      int col = idx % COLS;
      int x = col * BLOCK_SIZE;
      int y = row * BLOCK_SIZE;

      // 生成随机 RGB565 颜色
      uint16_t randColor = random(0, 65536);

      // 在内存中填充 Sprite 缓冲区
      block.fillSprite(randColor);
      // 画白色边框
      block.drawRect(0, 0, BLOCK_SIZE, BLOCK_SIZE, TFT_WHITE);

      // 一次性推送到 LCD
      block.pushSprite(x, y);
    }

    // 单帧结束时间
    unsigned long frameEnd = micros();
    unsigned long frameUs = frameEnd - frameStart;
    float frameMs = frameUs / 1000.0;
    float fps = 1000.0 / frameMs;

    frameCount++;

    // 每秒通过串口输出一次 FPS
    unsigned long now = millis();
    if (now - lastSerialTime >= 1000)
    {
      Serial.print("Frame: ");
      Serial.print(frameCount);
      Serial.print("  Time: ");
      Serial.print(frameMs);
      Serial.print(" ms  FPS: ");
      Serial.println(fps);
      lastSerialTime = now;
    }
  }

  // 不会执行到这里
  block.deleteSprite();
}
