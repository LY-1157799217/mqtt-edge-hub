// ============================================================
// integrated-balance.ino — SDD 小电视 整合版 (Step2)
//                       ★ 「Pi hub 近似平衡版」+ 天气节流修复 ★
//
// 【这份文件是什么】见同目录 README.md。一句话：
//   git 提交 5ad7843 的逐字节复刻（用户实测"模式切换流畅"的那个状态），
//   加上 2026-09-17 的【天气节流闸门修复】（PERFORMANCE.md 附录18）。
//
// 与 5ad7843 的差异（仅此 7 处，其余逐字节相同）：
//   ① 文件名 integrated.ino → integrated-balance.ino（Arduino 要求文件夹同名）
//   ② g_weatherNextFetch / g_weatherFailCount / g_weatherForce 提到文件作用域
//   ③ setMode 天气分支 + 切城市：改为"有缓存立即画 / 无缓存才取数 / 切城市强制刷新"
//   ④ 启动横幅加一行 [FW] 标识（仅打印，零行为改动）
//   ⑤ piHubHost 改为「宏默认值 + NVS + 网页可配」（原先硬编码 ⇒ 换网络就得重刷固件）
//   ⑥ 日K按需拉取：拆掉恒假判据 refreshIdx==stockCurIdx，改独立闸门 + 每股一份 TTL
//      （同时加 didFetch 互斥，保证"一轮最多 1 次取数"这条铁律不被打破）
//   ⑦ Pi 地址改 NVS + 网页可配（原来是硬编码 ⇒ 换网就得重刷固件）
//   ⑧ httpsGet10 加"裁到第一个 {"兜底（INC-003 第一层）+ 直连按TTL补分时图
//
// 四模式: 时钟/天气/相册(真实渲染) + 股票(占位, Step3)
// 配网: WiFiManager | 切模式: WebServer | 配置: Preferences
// 引脚(已在 User_Setup.h 配好): SCL=IO3 SDA=IO4 DC=IO2 RST=IO5 BL=IO1 CS=GND
// ============================================================

// 固件标识（见 README.md；改行为时必须同步改它）
#define FW_TAG "balance-fix5"   // fix4 + 直连补分时图(含 httpsGet10 兜底)（附录22）
#include <Arduino.h>
#include <ArduinoJson.h>         // 必须在 ESPAsyncWebServer.h 之前(否则库依赖扫描漏掉它)
#include <WiFi.h>
#include <ESPAsyncWebServer.h>  // 异步 Web 服务器（替代 WebServer）
#include <WiFiManager.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <SPIFFS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>            // 直接问驱动要真实的省电模式状态(esp_wifi_get_ps)，
                                 // 不用 WiFi.getSleep() —— 那个返回的是 Arduino 的缓存值
#include "number_alpha.h"  // alpha 数字字体(纯二值, 无JPEG伪影)
#include "weathernum.h"    // 天气图标/数字
#include "img/humidity.h"  // 湿度图标

TFT_eSPI tft = TFT_eSPI();
Preferences prefs;
AsyncWebServer server(80);         // 异步 Web 服务器
fs::File fsUploadFile;             // 网页上传文件句柄
String uploadError = "";            // 上传错误信息
bool uploadSuccess = false;         // 上传成功标志
size_t uploadedBytes = 0;           // 本次上传累计字节(异步分块需自行累计)
unsigned long pendingRestartAt = 0; // 非0=到点后重启(异步，避免在回调里 delay)
bool pendingWiFiReset = false;      // 重启前是否清 NVS + WiFi 凭据

// ---- INC-008 埋点：跨软件复位存活的面包屑（RTC 内存不被复位清零）----
// 背景：并发压测中设备反复静默重启(rst:0x3)，串口里既没有 Guru/panic，
//       也没有我们任何一处理应打印的日志。串口本身已证实会丢行（见 §7 INC-008），
//       所以不能只靠"日志里没看到"就下结论 —— 用 RTC 变量把死前状态带过复位边界。
// 判读：下次启动时看 [BOOT] 的"上次死前 loop阶段"。
RTC_NOINIT_ATTR uint32_t g_rtcMagic;      // 哨兵：不等于 RTC_MAGIC = 首次/彻底掉电
RTC_NOINIT_ATTR uint32_t g_rtcBoots;      // 累计启动次数(掉电才归零)
RTC_NOINIT_ATTR uint32_t g_rtcStage;      // 上次 loop 走到哪个阶段
RTC_NOINIT_ATTR uint32_t g_rtcStageFree;  // 到达该阶段时的 free
RTC_NOINIT_ATTR uint32_t g_rtcStageMax;   // 到达该阶段时的 maxAlloc
RTC_NOINIT_ATTR uint32_t g_rtcPanicBoots; // 【只统计真崩溃】的累计次数
RTC_NOINIT_ATTR uint32_t g_rtcLastReset;  // 本次启动的复位原因(esp_reset_reason 原始值)
// ⚠️ 为什么要把"崩溃次数"和"启动次数"分开：
//    g_rtcBoots 数的是【所有复位】—— 包括烧录时 Arduino IDE 拉 DTR/RTS 打的复位、
//    开关串口监视器打的复位。这些跟"设备自己崩了"完全是两回事，混在一起会误导判断。
//    实测踩过：看到 boots=4 就以为崩了 3 次，其实那几次大概率是烧录工具打的。
// ⚠️⚠️ 每次【新增 RTC_NOINIT_ATTR 变量】都必须改这个魔数！
//    RTC 内存不被复位清零 —— 这是它的优点，也是它的陷阱：
//    老固件的魔数还在 → 校验通过 → 新变量【跳过初始化】→ 读到的是上次残留的垃圾。
//    实测踩过：加了 g_rtcPanicBoots 却没改魔数，/status 返回 panicBoots=1070179764。
//    魔数在这里兼职"结构版本号"：改了它，下一次启动就会强制把所有 RTC 变量重新初始化。
#define RTC_MAGIC 0x5DD5DD60UL   // v2：新增 panicBoots / lastReset

// ⚠️⚠️ 绝对不要用 Serial.flush()！本机 FQBN 是 CDCOnBoot=cdc，Serial 是 USB CDC。
//   esp32 core 2.0.4 的 HWCDC::flush() 是一个【没有超时】的 while 循环：
//       while(uxItemsWaiting){ delay(5); vRingbufferGetInfo(...); }
//   串口监视器一关，主机就不读 → ringbuffer 永远排不空 → flush 永不返回。
//   实测现象：关掉监视器 → 设备黑屏；重新上电也没用；只有再打开监视器才恢复。
//   （因为 setup() 里 flush 卡在 tft.init() 之前，屏幕根本没初始化。）
//   HWCDC::write() 反而是有超时的（tx_timeout_ms 默认 200ms），只有 flush 是无限的。
//   要"把日志尽量发出去"就用下面这个【有上限】的等价物。
//
// ⚠️ 必须写成宏，不能写成函数！Arduino IDE 会把自动生成的原型
//    插在本文件【第一个函数定义】的位置。若这里是个函数，插入点就被顶到
//    第 54 行 —— 而 enum Mode 在 73 行、struct Quote 在 141 行还没定义，
//    于是满屏 "error: 'Quote' has not been declared"。宏不参与原型生成，绕开该坑。
#define serialDrain(ms) do { unsigned long _sdT0 = millis(); \
                             while (millis() - _sdT0 < (unsigned long)(ms)) delay(1); } while (0)
bool pendingWiFiSwitch = false;     // 待执行 WiFi 切换测试(阻塞操作，放 loop 里做)
String pendingNewSSID = "";
String pendingNewPass = "";

#define COL_BG   0x0000
#define COL_TEXT 0xFFFF
#define COL_DIM  0x7BEF

// ---------------- 背光PWM ----------------
#define BL_PIN      1     // GPIO1 背光引脚
#define BL_CHANNEL  0     // LEDC通道0
#define BL_FREQ     5000  // PWM频率5kHz
#define BL_RES      8     // 8位分辨率(0-255)

// ---------------- 模式 ----------------
enum Mode { MODE_CLOCK = 0, MODE_WEATHER = 1, MODE_PHOTO = 2, MODE_STOCK = 3 };
const char* MODE_NAMES[]    = {"时钟", "天气", "相册", "股票"};
const char* MODE_NAMES_EN[] = {"CLOCK", "WEATHER", "PHOTO", "STOCK"};
Mode currentMode = MODE_CLOCK;

// ---------------- 配置 ----------------
String cityCode        = "101120301";
String cityLabel       = "Zi Bo";     // 城市显示名称(拼音,可配置)
String stockCode       = "sh600519";
int    brightness      = 50;
bool   autoBrightness  = false;  // 自动亮度调节开关
int    defaultMode     = 0;
int    refreshInterval = 5;
int    wallpaperMode   = 1;   // 壁纸模式: 0无 1静态 2动态
int    wallpaperIndex  = 0;   // 静态壁纸索引(0-2)

// ---------------- 树莓派 Hub 配置 ----------------
// 【2026-09-17 balance-fix2】Pi 地址由硬编码改为可配置。
//   原因：写死的地址一旦换网络/换热点被重新分配就失效 ⇒ Pi 路径全程失败 ⇒ 永远走直连兜底
//   ⇒ 直连不提供 spark ⇒ 分时图空白（PERFORMANCE.md 附录19 的根因）。
//   ⚠️ 硬编码 IP 是【本版的历史局限】：换网络/热点重分配就会失效。
//      主线早已改成 NVS + 网页可配（提交 924999c）——那是本课题的【第二步】，本次不动。
// 【2026-09-17 balance-fix4】Pi 地址默认值 —— 仅作【兜底】。
//   实际取值优先来自 NVS（见 loadConfig），网页可改（`/set?pihub=`）。
//   本宏只在 NVS 里没有记录时使用（首次烧录 / NVS 被清 / 存了空串）。
//
//   为什么要有这个改动（PERFORMANCE.md 附录21）：
//     地址原来是硬编码的 ⇒ **换网络 / 手机热点重新分配 IP 就失效，只能重刷固件**。
//     这是附录19"第一步"就写明了的已知局限，本步把它补掉。
//   设计照抄主线的已验证实现（提交 924999c），不自己发明。
//   ⚠️ 公开仓库里本宏【留空】：这属于使用者自己的网络配置，不能带作者家里的地址。
//      留空 = 未配置 ⇒ Pi 路径必然失败 ⇒ 设备自动走直连兜底（不崩，只是没有 Pi 加速）。
//      首次烧录后请到网页控制台「树莓派 Hub 地址」填入你自己的 Pi IP。
#define PI_HUB_HOST_DEFAULT ""
String piHubHost       = PI_HUB_HOST_DEFAULT;  // 树莓派IP（loadConfig() 会用 NVS 覆盖它）
int    piHubPort       = 5000;

// ── 【静态 IP：本版【没有】，但主线已完整实现 —— 将来有固定 WIFI 时可直接搬，★不必重设计★】──
//
//   为什么本版没有（三条，见 PERFORMANCE.md 附录21）：
//     ① 用户已判定："**手机热点下基本没用（甚至有害）**，固定 WIFI 下比较有用"；
//        主线里它也是【默认关闭】的。
//     ② 它与 Pi 地址是【方向相反】的两条线：
//          静态 IP  管的是「**ESP32 自己的地址**会不会变」⇒ 影响 Pi / 企微找不找得到它
//          Pi 地址  管的是「**ESP32 能不能找到 Pi**」  ⇒ 影响能不能取到股票/天气数据
//        混在同一次改动里会违反"一次只动一个变量"。
//     ③ 当前不需要：网页控制端 + 企微控制都正常连上 ⇒ DHCP 分配是稳定的。
//
//   ★ 将来若真有了固定 WIFI 想启用它：**照下面这把清单搬到本文件即可，不要从零设计** ★
//     出处提交：`924999c`（"piHubHost 改 NVS+网页可配 + 静态IP(默认关) + 主动上报IP"）
//     落点（行号是主线 arduino/integrated/integrated.ino 当时的编号，可能已漂移 → 按内容找）：
//       :150-152   变量声明 staticIp / staticGw / staticSn / useStaticIp
//       :339-345   loadConfig() 里读 NVS + ★那段【风险说明注释】务必一起搬
//       :579-597   WiFi 连接时应用：三字段 IPv4 校验，**不合法则退回 DHCP**
//       :732-750   网页"静态 IP（一般不用改）"折叠卡片
//       :998-1031  处理器：开关 + 三字段 + 校验 + 写 NVS
//       :2731      /status 暴露 staticIp
//
//   ⚠️ 搬之前先读主线 loadConfig() 里那段风险注释，核心是：
//      **手机热点网段会变；静态 IP 换段后【完全连不上】，需要人工救回。**
//      所以它必须保持"默认关闭 + 网页可开"的形态，★绝不能改成默认启用★。
bool   usePiHub        = true;            // 是否使用树莓派Hub（可Web配置）
unsigned long piHubFailCount = 0;         // 连续【TCP连接】失败次数（不再含读超时/解析失败，见 piHubNoteTransportFail）
bool   piHubOffline    = false;           // Hub离线标志（连续3次失败标记）

// ---- Pi 请求耗时统计（验证"2 秒预算被自己吃掉"的推测，见 PERFORMANCE.md 附录）----
// 背景：PC 侧实测 Pi 失败率仅 0.6%，而 ESP32 侧 12 秒内连中 3 次失败（概率 ~2e-7）。
//       数学上不可能是同一件事 → 怀疑设备侧的【有效耗时】远高于纯网络耗时：
//       请求从 loop() 同步发起，同一个循环还在做渲染 / JPEG 解码 /
//       直连 HTTPS 兜底（TLS 握手在单核 C3 上阻塞 1~3 秒）。
// 读法：[PI] 行每 60s 打一次、打完清零 → 每个窗口独立可比。耗时是【墙钟】，
//       已经包含 CPU 被别的活占住的时间，这正是要量的东西。
//   行情 avg 远高于 PC 侧 p50(40ms) ⇒ 证实"预算被自身占用吃掉"
//   行情 avg 与 PC 侧相当          ⇒ 推测错误，回头查 WiFi / HTTP 客户端
static uint32_t piN = 0, piOk = 0, piFail = 0, piSlow = 0, piMax = 0;
static uint64_t piSum = 0;
// 分段计时：实测总耗时 ~1645ms，其中 GET 段占 97.6%、body 只占 2.4%
//（即 12KB 传输不是瓶颈，瓶颈在【连接阶段】）。所以改成把连接单独拆出来量：
//   conn = WiFiClient::connect()  ★纯 TCP 连接（绕开 hostByName 后量得准）
//   http = http.begin()+GET()     发请求 + 收状态行/响应头
//   body = http.getString()       收 12KB 响应体 —— 仅成功路径有
//   end  = http.end()             连接拆除
static uint64_t piSumConn = 0, piSumHttp = 0, piSumBody = 0, piSumEnd = 0;
static uint32_t piMaxConn = 0, piMaxHttp = 0, piMaxBody = 0;  // 各段最慢值，用来抓尾部
static uint32_t dcN = 0, dcMax = 0;       // 直连 HTTPS 兜底
static uint64_t dcSum = 0;
#define PI_SLOW_MS 1000UL                 // 超过它算"慢"，单独计数

// ── Pi Hub 的【读】超时（三个取数路径共用：行情 / 日K / 天气）──
// 【2026-09-14 实验 A 结论】2000 → 5000 → **3000**（定稿值）。
//
// 实验 A 证伪了「失败=延迟尾部越过阈值」这个假设：
//   把阈值抬到 5000 后，失败【数量一条没少(19→19)】，耗时只是整体跟着阈值跑
//   （http 段 p50 从 2032ms 变成 5042ms）—— 说明这批请求是【卡住】，不是【慢】。
//   而诚实的慢请求根本没有长尾：~2100 个样本里 http 段 p50=1128ms、p99=2260ms、>3000ms 为 0。
//   ⇒ 阈值抬到 5000 的唯一效果是【让每次卡死的阻塞从 2 秒变成 5 秒】，没有任何收益。
//
// 定 3000 的依据：诚实响应的 http 段 p99=2260ms，3000 留出余量；
//   同时把卡死的阻塞代价压到 3 秒。**这是止损值，不是"修好了"** ——
//   卡死本身的原因至今未知（PC 侧同窗口也有 3%，说明不是 ESP32 独有）。
//
// ⚠️ 超时值本身【不影响存活判定】—— 存活只看 TCP 能不能连上（见 piHubNoteTransportFail）。
// ⚠️ 三个路径必须一致，否则 PC 侧 probe 的对照会失去意义（tools/pi_hub_probe.py）。
#define PI_HTTP_TIMEOUT_MS 3000

// ---------------- Web HTML 静态模板(存Flash节省RAM) ----------------
const char HTML_HEADER[] PROGMEM = R"(<!DOCTYPE html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>SDD 小电视</title>
<style>
body{font-family:-apple-system,sans-serif;max-width:480px;margin:0 auto;padding:14px;background:#1a1a2e url('/m.jpg') no-repeat center center fixed;background-size:cover;color:#eee;min-height:100vh;box-sizing:border-box;}
.card{background:rgba(22,33,62,.9);border-radius:12px;padding:12px;margin:10px 0;}
h3{text-align:center;margin:4px 0;}
h4{margin:4px 0;}
.grid{display:flex;flex-wrap:wrap;gap:8px;}
button{font-size:18px;padding:10px 16px;border:none;border-radius:8px;background:#0f3460;color:#eee;cursor:pointer;flex:1;min-width:80px;}
button:active{background:#16537e;}
a{text-decoration:none;}
details.card>summary{font-size:18px;font-weight:bold;cursor:pointer;padding:6px;list-style:none;}
details.card>summary::before{content:'\25B8 ';}
details.card[open]>summary::before{content:'\25BE ';}
.sub{font-size:14px;color:#9ab;margin-top:8px;}
.center{text-align:center;}
</style></head><body>
<h3>SDD 小电视控制台</h3>
)";

struct tm timeinfo;

// ---------------- 天气数据 ----------------
struct Weather {
  float temp = 0; int humi = 0; int press = 0; int aqi = 0; int code = 99; bool ok = false;
};
Weather w;

// ── 【2026-09-17 修复】天气节流闸门的状态（原为 renderWeather 内的 static）────────
//   提到文件作用域，是因为【两个调用点】需要能绕过/重置它：
//     setMode() 的天气分支（进模式本该"必须发生"）
//     handleSet() 切城市（切了城市本该"必须刷新"）
//   完整病因说明见 renderWeather() 上方的注释块。
//   ⚠️ 这是本项目第 4 次踩「static 节流缓存在模式切换时没重置」那个坑，
//      按既有修法：**节流缓存一律放全局，由 setMode / 事件入口统一重置**。
static unsigned long g_weatherNextFetch = 0;      // 下次允许尝试的时刻（0 = 开机立即取一次）
static uint8_t       g_weatherFailCount = 0;      // 连续失败计数（退避用）
static bool          g_weatherForce     = false;  // 一次性：置位后 renderWeather 无条件执行一次
WeatherNum wrat;

// ===== 股票模块(多股轮动 + 分时/日K双视图) =====
#define SPARK_POINTS 48
#define KLINE_COUNT  30
#define SYMBOL_COUNT 4

struct Sym { const char* code; const char* label; };
Sym SYMBOLS[SYMBOL_COUNT] = {
  {"sh000001", "SZZS"},   // 上证指数
  {"sh600519", "GZMT"},   // 贵州茅台
  {"sz300308", "ZJXC"},   // 中际旭创
  {"sz000725", "JDFA"}    // 京东方A
};
// 启动时从 NVS 加载股票代码，没有就用上面的默认值

struct Quote {
  float price = 0, change = 0, changePct = 0, prevClose = 0, high = 0, low = 0;
  float spark[SPARK_POINTS];       // 分时采样点
  int   sparkLen = 0;
  float kl[KLINE_COUNT][4];        // 日K: [i][0]=开 [1]=收 [2]=高 [3]=低
  int   klCount = 0;
  bool ok = false;                 // 行情
  bool sparkOk = false;            // 分时
  bool klOk = false;               // 日K
};
Quote quotes[SYMBOL_COUNT];
int stockCurIdx = 0;               // 当前轮播索引
int stockView = 0;                 // 0=分时图 1=日K图

// ── 【2026-09-17 附录20】日K按需拉取的"每股一份"时间戳 ──────────────────────
//   原来日K的拉取判据是 `refreshIdx == stockCurIdx`，在 refreshInterval=20 时
//   【结构性恒假】（轮询周期 20*1000/4 = 5000ms == 轮播周期 5000ms，且轮播先跑
//   ⇒ 两个计数器永远差 1）⇒ 日K【一次都没拉过】。详见 PERFORMANCE.md 附录20。
//
//   ⚠️ 为什么必须【每股一份】而不是一个全局戳：轮播每 5 秒换一只，
//      全局戳会变成"只有一只的日K是新的、另外三只永远空白"
//      —— 与上面那个共振是同一类错误。见记忆 periodic-gate-resonance。
//
//   ⚠️ 日K一天才出一根 ⇒ TTL 给长毫无损失。
unsigned long klineStamp[SYMBOL_COUNT] = {0};   // 0 = 从未拉过（首次显示必定拉）
#define KLINE_TTL_MS 1800000UL                  // 30 分钟

// ── 【2026-09-17 balance-fix5 附录22】Pi 离线时，直连补分时图 ──────────────────
//   背景：直连的 fetchQuote 走 qt.gtimg.cn（明文 HTTP），**不提供 spark** ——
//         spark 是 Pi 的 quote JSON 里自带的。所以 Pi 离线时分时图空白。
//         补它需要一次 TLS（web.ifzq.gtimg.cn 是 302 强制 HTTPS）⇒ **重新触碰性能边界**。
//
//   ★可退开关★：置 0 即可回到"平衡状态"（直连不补分时图），不必改别的代码。
#define DIRECT_SPARK_BACKFILL 1
#define SPARK_TTL_MS 120000UL                   // 每股 2 分钟（比主线的 60s 更保守）

unsigned long sparkStamp[SYMBOL_COUNT] = {0};   // 0 = 从未拉过（首次显示必定拉）
unsigned long stockLastRotate = 0;

// alpha 数字字体(时钟, 无JPEG伪影)
TFT_eSprite numSpr(&tft);        // 36x60(时分)
TFT_eSprite numSprSmall(&tft);   // 18x30(秒)
TFT_eSprite wallpaperSpr(&tft);  // 壁纸缓存 240x240
TFT_eSprite dateSpr(&tft);       // 日期 sprite 120x24
TFT_eSprite tempSpr(&tft);       // 温度大字 sprite 160x40
TFT_eSprite textSpr(&tft);       // 小字 sprite 160x24(城市/湿度/气压/AQI)
TFT_eSprite chartSpr(&tft);      // 股票走势图帧缓冲 224x80(16bpp 35KB, 原子推屏)
bool wallpaperFreed = false;     // 股票模式是否已释放壁纸缓存(腾 115KB 给 HTTPS)
bool chartSprFreed = false;      // 时钟/天气模式是否已释放图表缓冲(腾 35KB 给壁纸重建)
bool smallFreed = false;         // 股票模式是否已释放小 sprite(时钟/天气数字文字缓冲, 腾 32KB)
#define COL_ORANGE 0xFD20
#define TRANSPARENT 0x0000
static const uint8_t* const A_O3660[10] = {A_O_3660_i0, A_O_3660_i1, A_O_3660_i2, A_O_3660_i3, A_O_3660_i4, A_O_3660_i5, A_O_3660_i6, A_O_3660_i7, A_O_3660_i8, A_O_3660_i9};
static const uint8_t* const A_W3660[10] = {A_W_3660_i0, A_W_3660_i1, A_W_3660_i2, A_W_3660_i3, A_W_3660_i4, A_W_3660_i5, A_W_3660_i6, A_W_3660_i7, A_W_3660_i8, A_W_3660_i9};
static const uint8_t* const A_W1830[10] = {A_W_1830_i0, A_W_1830_i1, A_W_1830_i2, A_W_1830_i3, A_W_1830_i4, A_W_1830_i5, A_W_1830_i6, A_W_1830_i7, A_W_1830_i8, A_W_1830_i9};

// ---------------- 相册 ----------------
const char* PHOTOS[] = {"/1.jpg", "/2.jpg", "/3.jpg"};
int curIdx = 0;
unsigned long lastRotate = 0;

// ---------------- 工具函数 ----------------
void loadConfig() {
  cityCode        = prefs.getString("cityCode", "101120301");
  cityLabel       = prefs.getString("cityLabel", "Zi Bo");
  stockCode       = prefs.getString("stockCode", "sh600519");
  brightness      = prefs.getInt("brightness", 50);
  autoBrightness  = prefs.getBool("autoBrightness", false);
  defaultMode     = prefs.getInt("defaultMode", 0);
  refreshInterval = prefs.getInt("refreshInterval", 5);
  wallpaperMode   = prefs.getInt("wallpaperMode", 1);
  wallpaperIndex  = prefs.getInt("wallpaperIndex", 0);

  // 树莓派 Hub 地址（换网段后靠它改回来，不用重刷固件 —— 见 PERFORMANCE.md 附录21）
  piHubHost       = prefs.getString("piHubHost", PI_HUB_HOST_DEFAULT);
  if (!piHubHost.length()) piHubHost = PI_HUB_HOST_DEFAULT;   // NVS 空串 ⇒ 仍为空（未配置，走直连兜底）
}

void setBacklight() {
  // 先不启用PWM，只用pinMode初始化
  pinMode(BL_PIN, OUTPUT);
  digitalWrite(BL_PIN, LOW);  // 默认全亮

  Serial.println("背光初始化: GPIO1, 模式=OUTPUT, 默认全亮");
  Serial.println("PWM测试模式：将在setBrightness()中尝试启用PWM");
}

// SPIFFS 启动清理(删除不在白名单的文件，保持存储干净)
void cleanupSPIFFS() {
  // 白名单：有效文件
  const char* VALID_FILES[] = {
    "/1.jpg", "/2.jpg", "/3.jpg",  // 相册3张
    "/m.jpg",                       // 网页壁纸
    "/city_data.json"               // 城市数据
  };

  // 遍历可能存在的旧文件并删除（直接尝试删除法，避免遍历API问题）
  const char* OLD_FILES[] = {
    "/4.jpg", "/5.jpg", "/6.jpg", "/7.jpg", "/8.jpg",  // 旧版相册残留
    "/test.jpg", "/temp.jpg", "/backup.jpg"             // 可能的测试文件
  };

  Serial.println("开始清理SPIFFS...");
  int deleted = 0;

  for (int i = 0; i < sizeof(OLD_FILES) / sizeof(OLD_FILES[0]); i++) {
    if (SPIFFS.exists(OLD_FILES[i])) {
      Serial.println("删除无用文件: " + String(OLD_FILES[i]));
      SPIFFS.remove(OLD_FILES[i]);
      deleted++;
    }
  }

  Serial.println("清理完成，删除了 " + String(deleted) + " 个文件");
}

// 背光亮度设置(0-100 → PWM占空比)
void setBrightness(int level) {
  if (level < 0) level = 0;
  if (level > 100) level = 100;

  if (level == 0) {
    // 完全关闭：先detach PWM，再用GPIO拉高
    ledcDetachPin(BL_PIN);
    pinMode(BL_PIN, OUTPUT);
    digitalWrite(BL_PIN, HIGH);  // P-MOS：高电平关闭
    Serial.println("背光: 关闭 (GPIO HIGH)");
  } else if (level == 100) {
    // 完全打开：先detach PWM，再用GPIO拉低
    ledcDetachPin(BL_PIN);
    pinMode(BL_PIN, OUTPUT);
    digitalWrite(BL_PIN, LOW);   // P-MOS：低电平全亮
    Serial.println("背光: 全亮 (GPIO LOW)");
  } else {
    // PWM调光：动态启用PWM
    ledcSetup(BL_CHANNEL, BL_FREQ, BL_RES);
    ledcAttachPin(BL_PIN, BL_CHANNEL);
    int duty = map(level, 0, 100, 255, 0);  // 反向映射
    ledcWrite(BL_CHANNEL, duty);
    Serial.printf("背光: %d%% (PWM duty=%d, 频率=%dHz)\n", level, duty, BL_FREQ);
  }

  brightness = level;  // 更新全局变量
}

// 自动亮度调节(根据时间段)
void autoAdjustBrightness() {
  if (!autoBrightness) return;  // 未开启自动调节，跳过
  if (!getLocalTime(&timeinfo)) return;  // 时间未同步，跳过

  int hour = timeinfo.tm_hour;
  int targetBrightness = brightness;

  // 时间段亮度策略
  if (hour >= 8 && hour < 12) {
    targetBrightness = 60;  // 08:00-11:59 → 60%
  } else if (hour >= 12 && hour < 15) {
    targetBrightness = 90;  // 12:00-14:59 → 90%
  } else if (hour >= 15 && hour < 20) {
    targetBrightness = 70;  // 15:00-19:59 → 70%
  } else if (hour >= 20 || hour < 8) {
    targetBrightness = 35;  // 20:00-07:59 → 35%
  }

  // 只在亮度变化时调整
  if (targetBrightness != brightness) {
    setBrightness(targetBrightness);
    Serial.printf("自动亮度: 时段%02d:xx → %d%%\n", hour, targetBrightness);
  }
}

// TFT输出回调(JPEG解码块推屏)
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y >= tft.height()) return 0;
  tft.pushImage(x, y, w, h, bitmap);
  return 1;
}

// 壁纸回调(解码到 wallpaperSpr)
bool tft_output_wallpaper(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  wallpaperSpr.pushImage(x, y, w, h, bitmap);
  return 1;
}

// 图标色键回调: 黑底转透明, 直接透明叠加到屏幕(壁纸)
bool tft_output_icon(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  for (int i = 0; i < w * h; i++) {
    uint16_t c = (bitmap[i] >> 8) | (bitmap[i] << 8);   // 交换回标准字节序(TJpgDec输出是交换序)
    uint8_t r = (c >> 11) & 0x1F;  r = (r << 3) | (r >> 2);
    uint8_t g = (c >> 5) & 0x3F;   g = (g << 2) | (g >> 4);
    uint8_t b = c & 0x1F;          b = (b << 3) | (b >> 2);
    uint8_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (mx < 50) bitmap[i] = 0x0000;   // 黑底+振铃+暗过渡(0-49)转透明
  }
  tft.pushImage(x, y, w, h, bitmap, 0x0000);   // 0x0000 不画, 叠到壁纸
  return 1;
}

// 解码相册图片到壁纸缓存
void loadWallpaper(int idx) {
  if (SPIFFS.exists(PHOTOS[idx])) {
    TJpgDec.setJpgScale(1);   // 1/1 不缩放
    TJpgDec.setCallback(tft_output_wallpaper);
    TJpgDec.drawFsJpg(0, 0, PHOTOS[idx]);
    TJpgDec.setCallback(tft_output);
  }
}

// 根据壁纸模式显示背景(无=纯黑, 静态=固定, 动态=当前索引)
void showWallpaper() {
  if (wallpaperMode == 0) {
    tft.fillScreen(COL_BG);          // 无壁纸, 屏幕纯黑
    wallpaperSpr.fillSprite(COL_BG); // 壁纸缓存也清黑(避免数字/天气读旧壁纸)
  } else {
    loadWallpaper(wallpaperIndex);
    wallpaperSpr.pushSprite(0, 0);
  }
}

// 壁纸缓存可能在股票模式被释放, 进入时钟/天气前重建(需在其他 sprite 都释放后再重建)
void ensureWallpaper() {
  if (wallpaperFreed) {
    wallpaperSpr.setColorDepth(16);
    if (wallpaperSpr.createSprite(240, 240) != nullptr) {
      wallpaperFreed = false;
    } else {
      Serial.printf("[wp] fail free=%d maxBlk=%d\n", ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    }
  }
}

// 图表缓冲可能在时钟/天气模式被释放, 进入股票前重建
void ensureChart() {
  if (chartSprFreed) {
    chartSpr.setColorDepth(16);
    if (chartSpr.createSprite(224, 80) != nullptr) {
      chartSprFreed = false;
    } else {
      Serial.println("[chart] createSprite fail");
    }
  }
}

// 释放小 sprite(时钟/天气的数字文字缓冲), 股票模式用不到, 腾堆
void freeSmallSprites() {
  if (!smallFreed) {
    numSpr.deleteSprite();
    numSprSmall.deleteSprite();
    dateSpr.deleteSprite();
    tempSpr.deleteSprite();
    textSpr.deleteSprite();
    smallFreed = true;
  }
}

// 重建小 sprite(时钟/天气需要)
void ensureSmallSprites() {
  if (!smallFreed) return;
  // 每个都检查返回值：任一失败就保持 smallFreed = true（下次还会重试），
  // 否则会重演 chartSpr 那个坑 —— 静默失败 + 标志被错误清掉 = 永不恢复。
  numSpr.setColorDepth(16);      bool okSpr = (numSpr.createSprite(36, 60)      != nullptr);
  numSprSmall.setColorDepth(16); okSpr = (numSprSmall.createSprite(18, 30) != nullptr) && okSpr;
  dateSpr.setColorDepth(16);     okSpr = (dateSpr.createSprite(120, 24)    != nullptr) && okSpr;
  tempSpr.setColorDepth(16);     okSpr = (tempSpr.createSprite(160, 40)    != nullptr) && okSpr;
  textSpr.setColorDepth(16);     okSpr = (textSpr.createSprite(160, 24)    != nullptr) && okSpr;
  if (!okSpr) {
    Serial.printf("[spr] 小 sprite 创建失败，保持待重建 (free=%u maxAlloc=%u)\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return;                       // smallFreed 保持 true → 下次仍会重试
  }
  smallFreed = false;
}

// ---------------- WiFiManager 配网 ----------------
void setupWifi() {
  WiFiManager wm;
  WiFiManagerParameter p_cc("CityCode", "城市代码", cityCode.c_str(), 9);
  WiFiManagerParameter p_bl("LCDBL", "屏幕亮度(1-100)", String(brightness).c_str(), 3);
  WiFiManagerParameter p_stock("StockCode", "股票代码", stockCode.c_str(), 9);
  WiFiManagerParameter p_mode("DefMode", "默认模式(0时钟1天气2相册3股票)", String(defaultMode).c_str(), 1);
  WiFiManagerParameter p_refresh("Refresh", "股票刷新间隔(秒)", String(refreshInterval).c_str(), 3);
  wm.addParameter(&p_cc); wm.addParameter(&p_bl); wm.addParameter(&p_stock);
  wm.addParameter(&p_mode); wm.addParameter(&p_refresh);
  wm.setConfigPortalTimeout(180);

  if (!wm.autoConnect("SDD小电视")) {
    Serial.println("配网失败, 重启");
    serialDrain(40);                 // 见 INC-008：复位前排空，否则这行会被丢掉
    ESP.restart();
  }

  // 关闭 WiFi 省电模式（出厂默认 WIFI_PS_MIN_MODEM）
  // 默认省电会让射频在 AP beacon 之间睡眠，收到的包由 AP 缓存、等下个 DTIM 才投递，
  // 而手机热点的 DTIM 周期常是 100~300ms → 每次 HTTP 往返都被拖这么久。
  // 怀疑它同时造成两个现象：① 首页 5KB 要 1.5 秒  ② Pi 请求 9.3% 失败(撞 2s 超时)。
  // 代价：功耗上升；本设备 USB 供电，可忽略。
  // 【验证要点】重启后首页应降到数百毫秒内；Pi 失败率应显著下降。
  //
  // ⚠️ 但"调过"不等于"生效"：WiFi.getSleep() 返回的是 Arduino 的【缓存值】，
  //    不是驱动的真实状态。这里直接问驱动 esp_wifi_get_ps()，把猜测变成实测。
  //    （PC 侧同 AP 同频段 TCP 握手 15ms，本机 526ms —— 差 35 倍，必须查清。）
  {
    wifi_ps_type_t psBefore = (wifi_ps_type_t)-1;
    esp_wifi_get_ps(&psBefore);
    WiFi.setSleep(false);
    wifi_ps_type_t psAfter = (wifi_ps_type_t)-1;
    esp_wifi_get_ps(&psAfter);
    // PS 读数：0=WIFI_PS_NONE(省电已关) / 1=MIN_MODEM / 2=MAX_MODEM
    Serial.printf("[WiFi] 省电模式 调用前=%d 调用后=%d (0=NONE 1=MIN_MODEM 2=MAX_MODEM)\n",
                  (int)psBefore, (int)psAfter);
  }
  Serial.println("[WiFi] 已关闭省电模式 (setSleep(false))");
  prefs.putString("cityCode", p_cc.getValue());
  prefs.putString("stockCode", p_stock.getValue());
  prefs.putInt("brightness", atoi(p_bl.getValue()));
  prefs.putInt("defaultMode", atoi(p_mode.getValue()));
  prefs.putInt("refreshInterval", atoi(p_refresh.getValue()));
  loadConfig();
  Serial.println("WiFi 已连接: " + WiFi.localIP().toString());
}

// ---------------- WebServer ----------------

// ---- 运行时观测（零风险：只读采样，不改变任何控制流）----
// uxTaskGetStackHighWaterMark(NULL) 中 NULL = 当前任务。
// Web 回调跑在 AsyncTCP 后台任务里，所以这里采到的就是它的栈余量。
//
// ⚠️ 单位坑（踩过）：ESP32 上返回值是【字节】，不是「字」。
//    原因：ESP-IDF 的 portmacro.h 里 `typedef uint8_t StackType_t`，
//    而 FreeRTOS 内部算的是 (end-start)/sizeof(StackType_t)，除数为 1 → 结果即字节数。
//    经典 32 位 FreeRTOS 才是"字(word, 4字节)"，ESP32 不是，不要乘 4。
//
// 返回值是任务创建以来的历史最小值(最坏情况)。因为 WaterMark 是任务级且单调取最坏值，
// 任何一处采样都能反映所有回调的最坏情况 —— 所以只需一两个采样点，不必给每个 handler 插桩。
static volatile uint32_t g_asyncStackHWM = 0xFFFFFFFF;

inline void sampleAsyncStack() {
  uint32_t hwm = uxTaskGetStackHighWaterMark(NULL);
  if (hwm < g_asyncStackHWM) g_asyncStackHWM = hwm;
}

void handleRoot(AsyncWebServerRequest *request) {
  sampleAsyncStack();
  // 使用 AsyncResponseStream 流式发送大型 HTML
  AsyncResponseStream *response = request->beginResponseStream("text/html; charset=utf-8");

  // 发送HTML头部（从Flash读取）
  response->print(FPSTR(HTML_HEADER));

  // 动态内容：当前状态
  char buf[512];
  snprintf(buf, sizeof(buf),
    "<div class='card center'>当前模式: <b>%s</b> &middot; IP: %s</div>",
    MODE_NAMES[currentMode], WiFi.localIP().toString().c_str());
  response->print(buf);

  // 模式切换
  response->print(F("<div class='card'><h4>模式切换</h4><div class='grid'>"));
  response->print(F("<a href='/set?mode=0'><button>时钟</button></a>"));
  response->print(F("<a href='/set?mode=1'><button>天气</button></a>"));
  response->print(F("<a href='/set?mode=2'><button>相册</button></a>"));
  response->print(F("<a href='/set?mode=3'><button>股票</button></a>"));
  response->print(F("</div></div>"));

  // 树莓派 Hub 地址折叠区（【2026-09-17 balance-fix4】附录21）
  //   这个地址原来是硬编码在固件里的 ⇒ 每次换网络/热点重分配都得改代码重刷固件。
  //   有了这个输入框，换网后 30 秒改完。
  response->print(F("<details class='card'><summary>树莓派 Hub 地址</summary>"
    "<form action='/set' method='get' style='margin-top:12px;display:flex;gap:8px'>"
    "<input name='pihub' maxlength='15' placeholder='192.168.1.100' "
    "style='flex:1;background:#0f0f1a;color:#eee;border:1px solid #444;"
    "border-radius:6px;padding:8px' value='"));
  response->print(piHubHost);
  response->print(F("'><button type='submit'>保存</button></form>"
    "<div class='sub'>换网络/换网段后改这里，<b>不用重刷固件</b>。"
    "保存后熔断器状态会自动复位，下一次取数即生效。</div></details>"));

  // 壁纸折叠区
  response->print(F("<details class='card'><summary>壁纸</summary><div class='grid'>"));
  response->print(F("<a href='/wp_select'><button>静态壁纸</button></a>"));
  response->print(F("<a href='/set?wp=2'><button>动态壁纸</button></a>"));
  response->print(F("<a href='/set?wp=0'><button>关闭壁纸</button></a>"));
  response->print(F("</div></details>"));

  // 股票视图折叠区
  response->print(F("<details class='card'><summary>股票视图</summary><div class='grid'>"));
  response->print(F("<a href='/set?stockview=0'><button>分时图</button></a>"));
  response->print(F("<a href='/set?stockview=1'><button>日K图</button></a>"));
  response->print(F("</div>"));
  snprintf(buf, sizeof(buf), "<div class='sub'>当前视图: %s</div></details>",
    stockView == 0 ? "分时图" : "日K图");
  response->print(buf);

  // 股票刷新间隔折叠区
  // 这个参数原本只存在于配网门户(WiFiManager 的 AP 模式)，日常根本够不着 ——
  // 但它直接决定 loop() 被同步 HTTP 占用多少，是当前最重要的性能旋钮。
  response->print(F("<details class='card'><summary>股票刷新间隔</summary><div class='grid'>"));
  response->print(F("<a href='/set?refresh=5'><button>5 秒</button></a>"));
  response->print(F("<a href='/set?refresh=10'><button>10 秒</button></a>"));
  response->print(F("<a href='/set?refresh=20'><button>20 秒</button></a>"));
  response->print(F("<a href='/set?refresh=30'><button>30 秒</button></a>"));
  response->print(F("<a href='/set?refresh=60'><button>60 秒</button></a>"));
  response->print(F("</div>"));
  snprintf(buf, sizeof(buf),
    "<div class='sub'>当前: %d 秒（每只股票 %.2f 秒取一次）</div></details>",
    refreshInterval, refreshInterval / (float)SYMBOL_COUNT);
  response->print(buf);

  // 屏幕亮度调节
  response->print(F("<details class='card'><summary>屏幕亮度</summary><div style='margin-top:12px'>"));

  // 自动亮度开关
  snprintf(buf, sizeof(buf),
    "<div style='margin-bottom:12px;padding:8px;background:rgba(15,52,96,0.5);border-radius:6px'>"
    "<label style='display:flex;align-items:center;cursor:pointer'>"
    "<input type='checkbox' id='autoBr' %s onchange='location.href=\"/set?auto_brightness=\"+(this.checked?1:0)' "
    "style='width:20px;height:20px;margin-right:8px'>"
    "<span>自动亮度调节</span></label>"
    "<div class='sub' style='margin-top:4px;font-size:12px'>08:00→60%% | 12:00→90%% | 15:00→70%% | 20:00→35%%</div>"
    "</div>",
    autoBrightness ? "checked" : "");
  response->print(buf);

  // 手动亮度滑块
  snprintf(buf, sizeof(buf),
    "<input type='range' min='0' max='100' value='%d' id='brightness' "
    "style='width:100%%;height:8px;border-radius:4px;outline:none;background:#0f3460' "
    "oninput='document.getElementById(\"brValue\").innerText=this.value'>"
    "<div style='text-align:center;margin-top:8px;font-size:20px;color:#38bdf8'>"
    "<span id='brValue'>%d</span>%%</div>"
    "<button onclick='location.href=\"/set?brightness=\"+document.getElementById(\"brightness\").value' "
    "style='width:100%%;margin-top:8px'>手动调节亮度</button>"
    "</div></details>",
    brightness, brightness);
  response->print(buf);

  // 天气城市切换
  snprintf(buf, sizeof(buf),
    "<details class='card'><summary>天气城市</summary>"
    "<div class='sub' style='margin-bottom:8px'>当前: <b>%s</b> (%s) "
    "<a href='/city_list' style='color:#38bdf8'>[查询城市代码]</a></div>"
    "<div class='grid'>",
    cityLabel.c_str(), cityCode.c_str());
  response->print(buf);

  // 快捷城市按钮
  response->print(F("<a href='/set?city=101010100&label=Beijing'><button>北京</button></a>"));
  response->print(F("<a href='/set?city=101020100&label=Shanghai'><button>上海</button></a>"));
  response->print(F("<a href='/set?city=101280101&label=Gz'><button>广州</button></a>"));
  response->print(F("<a href='/set?city=101280601&label=Shenzhen'><button>深圳</button></a>"));
  response->print(F("<a href='/set?city=101210101&label=Hangzhou'><button>杭州</button></a>"));
  response->print(F("<a href='/set?city=101270101&label=Chengdu'><button>成都</button></a>"));
  response->print(F("<a href='/set?city=101110101&label=Xian'><button>西安</button></a>"));
  response->print(F("<a href='/set?city=101200101&label=Wuhan'><button>武汉</button></a>"));
  response->print(F("<a href='/set?city=101120301&label=Zi Bo'><button>淄博</button></a>"));
  response->print(F("<a href='/set?city=101250101&label=Jinan'><button>济南</button></a>"));
  response->print(F("</div></details>"));

  // 快捷功能
  response->print(F("<div class='card center'><p>"));
  response->print(F("<a href='/upload' style='color:#9ab'>上传图片...</a> &middot; "));
  response->print(F("<a href='/stock_edit' style='color:#9ab'>股票配置...</a> &middot; "));
  response->print(F("<a href='/spiffs_list' style='color:#9ab'>SPIFFS文件列表</a></p>"));
  response->print(F("</div>"));

  // WiFi管理折叠区
  response->print(F("<details class='card' style='background:rgba(120,30,30,.85);border:1px solid #f87171;margin-top:20px'>"));
  response->print(F("<summary style='color:#fca5a5'>⚠ WiFi 管理</summary>"));
  response->print(F("<div class='grid' style='margin-top:12px'>"));
  response->print(F("<a href='/change_wifi'><button style='background:#16a34a;color:#fff'>更换 WiFi</button></a>"));
  response->print(F("<button onclick=\"if(confirm('确认重置 WiFi？\\n\\n设备将重启并进入配网模式（AP 热点）。\\n\\n请在设备重启后（约 5 秒），手动连接热点 SDD小电视，再打开 192.168.4.1 重新配网。')){location.href='/reset_wifi'}\" style='background:#dc2626;color:#fff'>重置 WiFi</button>"));
  response->print(F("</div>"));
  response->print(F("<div class='sub' style='margin-top:8px'>更换: 输入新 WiFi 快速切换<br>重置: 清空配置进 AP 配网模式（保底）</div>"));
  response->print(F("</details>"));

  // 结束标签
  response->print(F("</body></html>"));
  request->send(response);
}

// 异步任务标志位（让阻塞操作在 loop 中延迟执行）
volatile bool pendingModeChange = false;
volatile Mode pendingMode = MODE_CLOCK;
volatile bool pendingWeatherRefresh = false;
volatile bool pendingStockRefresh = false;
volatile bool pendingBrightnessAdjust = false;

void handleSet(AsyncWebServerRequest *request) {
  sampleAsyncStack();
  // 切模式
  if (request->hasParam("mode")) {
    int m = request->getParam("mode")->value().toInt();
    if (m >= 0 && m < 4) {
      pendingMode = (Mode)m;
      pendingModeChange = true;  // 标记待执行，避免阻塞
      request->send(200, "text/html; charset=utf-8",
                    "已切换到 <b>" + String(MODE_NAMES[m]) + "</b>  <a href='/'>返回</a>");
      return;
    }
  }
  // 设壁纸模式
  if (request->hasParam("wp")) {
    int wp = request->getParam("wp")->value().toInt();
    if (wp >= 0 && wp <= 2) {
      wallpaperMode = wp;
      prefs.putInt("wallpaperMode", wp);
      if (request->hasParam("idx")) {          // 静态壁纸索引
        wallpaperIndex = request->getParam("idx")->value().toInt();
        prefs.putInt("wallpaperIndex", wallpaperIndex);
      }
      if (currentMode == MODE_CLOCK || currentMode == MODE_WEATHER) {
        pendingMode = currentMode;
        pendingModeChange = true;  // 异步重新显示背景
      }
      request->send(200, "text/html; charset=utf-8",
                    "壁纸已设置  <a href='/'>返回</a>");
      return;
    }
  }
  // 切换城市
  if (request->hasParam("city")) {
    String newCity = request->getParam("city")->value();
    String newLabel = request->hasParam("label") ? request->getParam("label")->value() : "";
    newCity.trim();
    newLabel.trim();
    if (newCity.length() >= 6 && newCity.length() <= 12) {  // 城市代码一般9位
      cityCode = newCity;
      prefs.putString("cityCode", cityCode);
      // 如果有label参数，保存label；否则保持旧label
      if (newLabel.length() > 0) {
        cityLabel = newLabel;
        prefs.putString("cityLabel", cityLabel);
      }
      // 【2026-09-17 修复同一个闸门的第二个发作点】
      //   原来：w.ok = false; + pendingWeatherRefresh = true; ⇒ 主循环里 renderWeather();
      //   ⇒ 同一个闸门 ⇒ 若距上次成功取数不足 10 分钟 ⇒ 【城市切了，天气不刷新】。
      //   现在：置"一次性强制"，让主循环里那一次无条件执行。
      w.ok = false;                    // 城市变了，旧数据作废
      if (currentMode == MODE_WEATHER) {
        g_weatherForce = true;         // 切城市必须立即刷新，不能等 TTL
        pendingWeatherRefresh = true;  // 异步刷新天气显示
      }
      request->send(200, "text/html; charset=utf-8",
                    "城市已切换到 <b>" + cityLabel + "</b> (" + cityCode + ")  <a href='/'>返回</a>");
      return;
    } else {
      request->send(400, "text/html; charset=utf-8",
                    "城市代码格式错误（应为6-12位数字）  <a href='/'>返回</a>");
      return;
    }
  }
  // 设股票视图(分时/日K)
  if (request->hasParam("stockview")) {
    int sv = request->getParam("stockview")->value().toInt();
    if (sv >= 0 && sv <= 1) {
      stockView = sv;
      if (currentMode == MODE_STOCK) {
        pendingStockRefresh = true;  // 异步重绘当前股
      }
      request->send(200, "text/html; charset=utf-8",
                    "股票视图已切换  <a href='/'>返回</a>");
      return;
    }
  }
  // ── 树莓派 Hub 地址（【2026-09-17 balance-fix4】附录21）────────────────────
  //   换网段后靠它改回来，不用重刷固件 —— 这是本次改动的全部意义。
  if (request->hasParam("pihub")) {
    String ph = request->getParam("pihub")->value();
    ph.trim();
    IPAddress tmp;
    if (ph.length() && tmp.fromString(ph)) {          // 必须是合法 IPv4
      String old = piHubHost;
      piHubHost = ph;
      prefs.putString("piHubHost", piHubHost);
      // ⚠️ 必须复位熔断器：之前"离线"是【基于旧地址】得出的结论，地址一换就作废了。
      //    不清的话，新地址要等最长 60 秒（HALF-OPEN 周期）才会被尝试一次
      //    ⇒ 用户会以为"改了没用"。
      piHubFailCount = 0;
      piHubOffline   = false;
      char msg[224];
      snprintf(msg, sizeof(msg),
        "树莓派地址已设为 <b>%s</b>（原 %s）<br>"
        "熔断器状态已复位，下一次取数即生效  <a href='/'>返回</a>",
        piHubHost.c_str(), old.c_str());
      request->send(200, "text/html; charset=utf-8", msg);
      return;
    }
    request->send(400, "text/html; charset=utf-8",
                  "地址不合法（应为 IPv4，如 192.168.1.100）  <a href='/'>返回</a>");
    return;
  }
  // 设置亮度
  if (request->hasParam("brightness")) {
    int br = request->getParam("brightness")->value().toInt();
    if (br >= 0 && br <= 100) {
      autoBrightness = false;  // 手动调节时关闭自动亮度
      prefs.putBool("autoBrightness", false);
      setBrightness(br);
      prefs.putInt("brightness", br);
      request->send(200, "text/html; charset=utf-8",
                    "亮度已设置为 <b>" + String(br) + "%</b> (自动亮度已关闭)  <a href='/'>返回</a>");
      return;
    }
  }
  // 自动亮度开关
  if (request->hasParam("auto_brightness")) {
    int ab = request->getParam("auto_brightness")->value().toInt();
    autoBrightness = (ab == 1);
    prefs.putBool("autoBrightness", autoBrightness);
    if (autoBrightness) {
      pendingBrightnessAdjust = true;  // 异步调整亮度
      request->send(200, "text/html; charset=utf-8",
                    "自动亮度已<b>开启</b>  <a href='/'>返回</a>");
    } else {
      request->send(200, "text/html; charset=utf-8",
                    "自动亮度已<b>关闭</b>  <a href='/'>返回</a>");
    }
    return;
  }

  // 股票刷新间隔(秒)
  // 原本只是 WiFiManager 的一个参数(存在 NVS 的 "refreshInterval")，只有进配网门户才能改，
  // 日常够不着。而它决定 loop() 里同步 HTTP 的占空比 —— 实测 5 秒时 loop() 每轮要 1 秒。
  // 单次取数耗时约 1.2 秒，而 5 秒 / 4 只 = 1.25 秒取一只 → 数学上不可能持续。
  if (request->hasParam("refresh")) {
    int ri = request->getParam("refresh")->value().toInt();
    if (ri >= 3 && ri <= 600) {
      refreshInterval = ri;
      prefs.putInt("refreshInterval", ri);
      char msg[160];
      snprintf(msg, sizeof(msg),
        "股票刷新间隔已设为 <b>%d 秒</b>（每只股票 %.2f 秒取一次）  <a href='/'>返回</a>",
        ri, ri / (float)SYMBOL_COUNT);
      request->send(200, "text/html; charset=utf-8", msg);
      return;
    }
    request->send(400, "text/html; charset=utf-8",
                  "刷新间隔需在 3~600 秒之间  <a href='/'>返回</a>");
    return;
  }
  request->send(400, "text/plain", "参数错误");
}

// 静态壁纸选图页(3张图总览)
void handleWallpaperSelect(AsyncWebServerRequest *request) {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>选择静态壁纸</title></head><body>";
  html += "<h3>选择一张作为静态壁纸</h3><p>";
  for (int i = 0; i < 3; i++) {
    String mark = (wallpaperMode == 1 && wallpaperIndex == i) ? " ✓" : "";
    html += "<a href='/set?wp=1&idx=" + String(i) + "'><button style='font-size:20px;margin:4px'>"
            "图片" + String(i + 1) + mark + "</button></a> ";
  }
  html += "</p><p><a href='/'>返回</a></p></body></html>";
  request->send(200, "text/html; charset=utf-8", html);
}

// 网页壁纸图片服务(SPIFFS 里的 m.jpg)
void handleImage(AsyncWebServerRequest *request) {
  if (SPIFFS.exists("/m.jpg")) {
    request->send(SPIFFS, "/m.jpg", "image/jpeg");
  } else {
    request->send(404, "text/plain", "not found");
  }
}

// 城市数据JSON服务(SPIFFS 里的 city_data.json)
void handleCityDataJSON(AsyncWebServerRequest *request) {
  if (SPIFFS.exists("/city_data.json")) {
    request->send(SPIFFS, "/city_data.json", "application/json");
  } else {
    request->send(404, "application/json", "{\"error\":\"city_data.json not found\"}");
  }
}

// 网页上传图片页(分区：网页壁纸直传 + 相册前端裁剪240x240)
void handleUploadPage(AsyncWebServerRequest *request) {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>上传图片</title>";
  html += "<link rel='stylesheet' href='https://cdn.jsdelivr.net/npm/cropperjs@1.6.1/dist/cropper.min.css'>";
  html += "<style>body{font-family:sans-serif;max-width:480px;margin:0 auto;padding:16px;background:#1a1a2e;color:#eee;}";
  html += "h3{margin:4px 0;} .card{background:rgba(22,33,62,.9);border-radius:12px;padding:12px;margin:10px 0;}";
  html += "label{font-size:14px;color:#9ab;display:block;margin-bottom:6px;}";
  html += "input[type=file]{color:#eee;font-size:14px;}";
  html += "button{font-size:16px;padding:8px 20px;border:none;border-radius:8px;background:#0f3460;color:#eee;cursor:pointer;margin-top:8px;}";
  html += ".btn-cancel{background:#555;margin-left:6px;}";
  html += "a{color:#9ab;} #cropModal{display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.85);z-index:999;padding:20px;box-sizing:border-box;}";
  html += "#cropContainer{max-width:400px;margin:0 auto;background:#16213e;border-radius:12px;padding:16px;} #cropPreview{max-width:100%;margin-bottom:12px;display:block;}</style></head><body>";
  html += "<h3>上传图片</h3>";
  html += "<p style='font-size:13px;color:#9ab'>上传后设备自动重启生效</p>";

  // 网页壁纸区(直传)
  html += "<div class='card'><b>网页壁纸</b><p style='font-size:12px;color:#9ab;margin:4px 0'>根据设备尺寸自选，无需裁剪</p>";
  html += "<form method='POST' action='/do_upload' enctype='multipart/form-data'>";
  html += "<input type='hidden' name='fname' value='m.jpg'>";
  html += "<label>选择图片（将保存为 m.jpg）</label>";
  html += "<input type='file' name='file' accept='image/*'><br>";
  html += "<button type='submit'>上传并重启</button></form></div>";

  // 城市数据区(JSON直传)
  html += "<div class='card'><b>城市数据</b><p style='font-size:12px;color:#9ab;margin:4px 0'>上传城市代码JSON文件，用于城市查询功能</p>";
  html += "<form method='POST' action='/do_upload' enctype='multipart/form-data'>";
  html += "<input type='hidden' name='fname' value='city_data.json'>";
  html += "<label>选择JSON文件（将保存为 city_data.json）</label>";
  html += "<input type='file' name='file' accept='.json,application/json'><br>";
  html += "<button type='submit'>上传并重启</button></form></div>";

  // 相册区(前端裁剪240x240)
  html += "<div class='card'><b>相册图片</b><p style='font-size:12px;color:#9ab;margin:4px 0'>屏幕尺寸 240×240，选图后可裁剪</p>";
  for (int i = 1; i <= 3; i++) {
    html += "<div style='margin-top:10px;border-top:1px solid #334;padding-top:8px'><label>图片" + String(i) + "（" + String(i) + ".jpg）</label>";
    html += "<input type='file' id='f" + String(i) + "' accept='image/*' style='margin-bottom:8px'>";
    html += "<button onclick='startCrop(" + String(i) + ")'>选图并裁剪</button></div>";
  }
  html += "</div>";

  // 裁剪弹窗
  html += "<div id='cropModal'><div id='cropContainer'>";
  html += "<h4 style='margin-top:0;color:#eee'>调整裁剪区域</h4>";
  html += "<img id='cropPreview'>";
  html += "<div style='margin-top:12px'><button onclick='uploadCropped()'>确认并上传</button>";
  html += "<button class='btn-cancel' onclick='cancelCrop()'>取消</button></div></div></div>";

  html += "<p><a href='/'>返回</a></p>";
  html += "<script src='https://cdn.jsdelivr.net/npm/cropperjs@1.6.1/dist/cropper.min.js'></script>";
  html += "<script>let cropper,curIdx;";
  html += "function startCrop(i){curIdx=i;const inp=document.getElementById('f'+i);const f=inp.files[0];";
  html += "if(!f){alert('请先选择图片');return;}const r=new FileReader();r.onload=e=>{";
  html += "const img=document.getElementById('cropPreview');img.src=e.target.result;";
  html += "document.getElementById('cropModal').style.display='block';";
  html += "if(cropper)cropper.destroy();setTimeout(()=>{cropper=new Cropper(img,{aspectRatio:1,viewMode:1,autoCropArea:1});},100);};r.readAsDataURL(f);}";
  html += "function cancelCrop(){document.getElementById('cropModal').style.display='none';if(cropper)cropper.destroy();}";
  html += "function uploadCropped(){if(!cropper){alert('裁剪未就绪');return;}cropper.getCroppedCanvas({width:240,height:240}).toBlob(blob=>{";
  html += "const fd=new FormData();fd.append('fname',curIdx+'.jpg');fd.append('file',blob,curIdx+'.jpg');";
  html += "document.getElementById('cropModal').style.display='none';document.body.innerHTML='<div style=\"text-align:center;padding:60px;color:#eee\"><h3>上传中...</h3></div>';";
  html += "fetch('/do_upload',{method:'POST',body:fd}).then(r=>r.text()).then(()=>{});";
  html += "},'image/jpeg',0.9);}</script>";
  html += "</body></html>";
  request->send(200, "text/html; charset=utf-8", html);
}

// 股票编辑页
void handleStockEdit(AsyncWebServerRequest *request) {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>更换股票</title>";
  html += "<style>body{font-family:sans-serif;max-width:480px;margin:0 auto;padding:16px;background:#1a1a2e;color:#eee;}";
  html += "input{font-size:16px;padding:8px;border:1px solid #555;border-radius:6px;background:#222;color:#eee;}";
  html += ".code{width:140px;margin-right:8px;} .label{width:80px;}";
  html += "button{font-size:18px;padding:10px 24px;border:none;border-radius:8px;background:#0f3460;color:#eee;cursor:pointer;margin-top:12px;}";
  html += ".hint{font-size:13px;color:#9ab;margin-top:4px;}</style></head><body>";
  html += "<h3>更换股票代码</h3>";
  html += "<p style='font-size:14px;color:#9ab'>前缀说明：<b>sh</b>=上海A股，<b>sz</b>=深圳A股，<b>hk</b>=香港股票<br>";
  html += "名称用拼音首字母，如：GZMT(贵州茅台)，ZJXC(中际旭创)</p>";
  html += "<form method='GET' action='/stock_save'>";
  for (int i = 0; i < SYMBOL_COUNT; i++) {
    html += "<p>股票" + String(i + 1) + ": ";
    html += "<input class='code' name='c" + String(i) + "' value='" + SYMBOLS[i].code + "' placeholder='代码' maxlength='12'> ";
    html += "<input class='label' name='l" + String(i) + "' value='" + SYMBOLS[i].label + "' placeholder='拼音' maxlength='8'>";
    html += "<div class='hint'>当前: " + String(SYMBOLS[i].code) + " / " + String(SYMBOLS[i].label) + "</div></p>";
  }
  html += "<button type='submit'>保存并重启</button>";
  html += "</form><p><a href='/' style='color:#9ab'>返回</a></p></body></html>";
  request->send(200, "text/html; charset=utf-8", html);
}

// 保存股票代码+名称到 NVS，重启生效
void handleStockSave(AsyncWebServerRequest *request) {
  Serial.println("=== 收到股票保存请求 ===");
  for (int i = 0; i < SYMBOL_COUNT; i++) {
    String argC = "c" + String(i);
    String argL = "l" + String(i);
    if (request->hasParam(argC) && request->hasParam(argL)) {
      String code = request->getParam(argC)->value();
      String label = request->getParam(argL)->value();
      code.trim();
      label.trim();
      Serial.printf("检查参数 %s=%s, %s=%s\n", argC.c_str(), code.c_str(), argL.c_str(), label.c_str());
      if (code.length() > 0 && label.length() > 0) {
        prefs.putString(("stock" + String(i)).c_str(), code);
        prefs.putString(("label" + String(i)).c_str(), label);
        Serial.printf("已保存 stock%d=%s, label%d=%s\n", i, code.c_str(), i, label.c_str());
      } else {
        Serial.printf("跳过 %d：code 或 label 为空\n", i);
      }
    } else {
      Serial.printf("跳过 %d：参数不存在\n", i);
    }
  }
  request->send(200, "text/html; charset=utf-8",
    "<html><body style='font-family:sans-serif;background:#1a1a2e;color:#eee;text-align:center;padding:40px'>"
    "<h3>已保存，设备将在 2 秒后重启</h3><p>重启后新股票生效</p></body></html>");
  pendingRestartAt = millis() + 2000;   // 异步延迟重启，先让响应发完
}

// WiFi 重置
void handleResetWiFi(AsyncWebServerRequest *request) {
  request->send(200, "text/html; charset=utf-8",
    "<html><body style='font-family:sans-serif;background:#1a1a2e;color:#eee;text-align:center;padding:40px'>"
    "<h3>WiFi 配置已清除</h3>"
    "<p>设备将在 2 秒后重启并进入配网模式（AP 热点）</p>"
    "<p style='color:#fbbf24;margin-top:20px'>请在设备重启后（约 5 秒），手动连接热点<br><strong>SDD小电视</strong><br>再打开 <strong>192.168.4.1</strong> 重新配网</p>"
    "</body></html>");
  pendingWiFiReset = true;              // 重启前清 NVS + WiFi 凭据
  pendingRestartAt = millis() + 2000;   // 异步延迟重启，先让响应发完
}

// WiFi 更换页面(表单)
void handleChangeWiFi(AsyncWebServerRequest *request) {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>更换 WiFi</title>";
  html += "<style>";
  html += "body{font-family:sans-serif;background:#1a1a2e;color:#eee;max-width:400px;margin:40px auto;padding:20px;}";
  html += "h3{text-align:center;margin-bottom:24px;}";
  html += "label{display:block;margin-top:16px;font-size:14px;color:#9ab;}";
  html += "input{width:100%;padding:10px;margin-top:4px;border:1px solid #456;border-radius:6px;background:#0f3460;color:#eee;font-size:16px;box-sizing:border-box;}";
  html += "button{width:100%;margin-top:24px;padding:12px;border:none;border-radius:8px;background:#16a34a;color:#fff;font-size:18px;cursor:pointer;}";
  html += "button:active{background:#15803d;}";
  html += ".back{margin-top:12px;background:#475569;}";
  html += ".back:active{background:#334155;}";
  html += "</style></head><body>";
  html += "<h3>更换 WiFi</h3>";
  html += "<form action='/do_change_wifi' method='GET'>";
  html += "<label>新 WiFi 名称 (SSID)</label>";
  html += "<input type='text' name='ssid' placeholder='输入WiFi名称' required>";
  html += "<label>新 WiFi 密码</label>";
  html += "<input type='password' name='pass' placeholder='输入密码' required>";
  html += "<button type='submit'>连接并保存</button>";
  html += "</form>";
  html += "<button class='back' onclick=\"location.href='/'\">返回</button>";
  html += "</body></html>";
  request->send(200, "text/html; charset=utf-8", html);
}

// WiFi 更换执行(安全连接测试)
void handleDoChangeWiFi(AsyncWebServerRequest *request) {
  if (!request->hasParam("ssid") || !request->hasParam("pass")) {
    request->send(400, "text/html; charset=utf-8",
      "<html><body style='font-family:sans-serif;background:#1a1a2e;color:#eee;text-align:center;padding:40px'>"
      "<h3>参数错误</h3><p><a href='/change_wifi' style='color:#38bdf8'>返回重试</a></p></body></html>");
    return;
  }

  String newSSID = request->getParam("ssid")->value();
  String newPass = request->getParam("pass")->value();
  newSSID.trim();
  newPass.trim();

  // 先返回页面(告知正在连接)
  request->send(200, "text/html; charset=utf-8",
    "<html><body style='font-family:sans-serif;background:#1a1a2e;color:#eee;text-align:center;padding:40px'>"
    "<h3>正在连接新 WiFi...</h3>"
    "<p style='color:#fbbf24'>请等待 30 秒<br>成功后设备将自动重启</p>"
    "<p style='font-size:14px;color:#9ab;margin-top:20px'>如果连接失败，旧 WiFi 保持不变</p>"
    "</body></html>");

  // 连接测试含 30 秒阻塞循环，交给 loop() 执行，避免卡住异步服务器
  pendingNewSSID = newSSID;
  pendingNewPass = newPass;
  pendingWiFiSwitch = true;
}

// 由 loop() 调用：实际执行 WiFi 切换测试(阻塞，最长约 30 秒)
void doWiFiSwitch(const String& newSSID, const String& newPass) {
  WiFi.disconnect();
  WiFi.begin(newSSID.c_str(), newPass.c_str());
  Serial.printf("尝试连接新 WiFi: %s\n", newSSID.c_str());

  int timeout = 30;
  while (WiFi.status() != WL_CONNECTED && timeout > 0) {
    delay(1000);
    Serial.print(".");
    timeout--;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n新 WiFi 连接成功: %s\n", WiFi.localIP().toString().c_str());
    // WiFi.begin 成功后 ESP32 会自动保存凭据到 NVS
    delay(1000);
    serialDrain(40);
    ESP.restart();  // 重启生效
  } else {
    Serial.println("\n新 WiFi 连接失败，恢复旧连接");
    WiFi.disconnect();
    setupWifi();  // 重新走 WiFiManager 连接已保存的旧 WiFi
  }
}

// 城市代码查询页面(带搜索功能，从SPIFFS加载city_data.json)
void handleCityList(AsyncWebServerRequest *request) {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>城市代码查询</title>";
  html += "<style>";
  html += "body{font-family:sans-serif;background:#1a1a2e;color:#eee;max-width:600px;margin:20px auto;padding:20px;}";
  html += "h3{text-align:center;margin-bottom:20px;}";
  html += "#search{width:100%;padding:12px;margin-bottom:16px;border:1px solid #456;border-radius:8px;background:#0f3460;color:#eee;font-size:16px;box-sizing:border-box;}";
  html += "#count{text-align:center;color:#9ab;margin-bottom:12px;font-size:14px;}";
  html += "table{width:100%;border-collapse:collapse;background:rgba(22,33,62,.9);border-radius:8px;overflow:hidden;}";
  html += "th,td{padding:10px;text-align:left;border-bottom:1px solid #456;}";
  html += "th{background:#0f3460;color:#9ab;font-weight:bold;position:sticky;top:0;}";
  html += "tr:last-child td{border-bottom:none;}";
  html += "tr.hidden{display:none;}";
  html += "a{color:#38bdf8;text-decoration:none;}";
  html += ".back{text-align:center;margin-top:20px;}";
  html += ".loading{text-align:center;padding:40px;color:#9ab;}";
  html += "</style></head><body>";
  html += "<h3>城市代码查询</h3>";
  html += "<input type='text' id='search' placeholder='输入城市名称或代码搜索...'>";
  html += "<div id='count'></div>";
  html += "<div id='loading' class='loading'>加载中...</div>";
  html += "<table id='cityTable' style='display:none'>";
  html += "<thead><tr><th>城市</th><th>代码</th></tr></thead>";
  html += "<tbody id='cityBody'></tbody>";
  html += "</table>";
  html += "<p class='back'><a href='/'>返回控制台</a></p>";
  html += "<script>";
  html += "let cities={};let total=0;";
  html += "fetch('/city_data.json').then(r=>r.json()).then(data=>{";
  html += "cities=data;total=Object.keys(data).length;";
  html += "const tbody=document.getElementById('cityBody');";
  html += "for(const[city,code]of Object.entries(data)){";
  html += "const tr=document.createElement('tr');tr.dataset.city=city;tr.dataset.code=code;";
  html += "tr.innerHTML=`<td>${city}</td><td>${code}</td>`;tbody.appendChild(tr);}";
  html += "document.getElementById('loading').style.display='none';";
  html += "document.getElementById('cityTable').style.display='table';";
  html += "updateCount();";
  html += "}).catch(()=>{document.getElementById('loading').innerHTML='<span style=\"color:#f87171\">加载失败，请确保已上传 city_data.json</span>';});";
  html += "document.getElementById('search').addEventListener('input',e=>{";
  html += "const q=e.target.value.toLowerCase();";
  html += "const rows=document.querySelectorAll('#cityBody tr');";
  html += "let visible=0;";
  html += "rows.forEach(row=>{";
  html += "const city=row.dataset.city.toLowerCase();const code=row.dataset.code;";
  html += "if(city.includes(q)||code.includes(q)){row.classList.remove('hidden');visible++;}";
  html += "else{row.classList.add('hidden');}});";
  html += "updateCount(visible);});";
  html += "function updateCount(v){const c=v===undefined?total:v;";
  html += "document.getElementById('count').textContent=`显示 ${c} / ${total} 个城市`;}";
  html += "</script>";
  html += "</body></html>";
  request->send(200, "text/html; charset=utf-8", html);
}

// SPIFFS 文件列表(临时调试路由)
void handleSPIFFSList(AsyncWebServerRequest *request) {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>SPIFFS 存储信息</title>";
  html += "<style>";
  html += "body{font-family:sans-serif;background:#1a1a2e;color:#eee;max-width:600px;margin:20px auto;padding:20px;}";
  html += "h3{text-align:center;margin-bottom:30px;}";
  html += ".info{background:rgba(22,33,62,.9);border-radius:12px;padding:20px;text-align:center;}";
  html += ".stat{font-size:18px;margin:12px 0;color:#9ab;}";
  html += ".stat b{color:#eee;font-size:24px;}";
  html += "a{color:#38bdf8;text-decoration:none;}";
  html += ".back{text-align:center;margin-top:30px;}";
  html += "</style></head><body>";
  html += "<h3>SPIFFS 存储信息</h3>";

  // 统计信息
  size_t totalBytes = SPIFFS.totalBytes();
  size_t usedBytes = SPIFFS.usedBytes();
  size_t freeBytes = totalBytes - usedBytes;
  int usedPercent = usedBytes * 100 / totalBytes;

  html += "<div class='info'>";
  html += "<div class='stat'>总容量: <b>" + String(totalBytes / 1024) + " KB</b></div>";
  html += "<div class='stat'>已使用: <b>" + String(usedBytes / 1024) + " KB</b> (" + String(usedPercent) + "%)</div>";
  html += "<div class='stat'>剩余空间: <b>" + String(freeBytes / 1024) + " KB</b></div>";
  html += "</div>";

  html += "<p class='back'><a href='/'>返回控制台</a></p>";
  html += "</body></html>";
  request->send(200, "text/html; charset=utf-8", html);
}

// ============================================================
//  时钟模式
// ============================================================
int lastH = -1, lastM = -1, lastS = -1, lastD = -1;   // 节流(全局, setMode重置强制重绘)

void drawColon() {
  tft.fillCircle(114, 74, 4, COL_TEXT);
  tft.fillCircle(114, 102, 4, COL_TEXT);
}

void drawDate() {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d.%d.%d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
  dateSpr.fillSprite(TRANSPARENT);
  dateSpr.setTextColor(COL_TEXT);
  dateSpr.setTextSize(2);
  int w = dateSpr.textWidth(buf);
  dateSpr.setCursor((dateSpr.width() - w) / 2, 0);
  dateSpr.print(buf);
  dateSpr.pushSprite((240 - w) / 2, 166, TRANSPARENT);   // 透明叠加到壁纸
}

// alpha 数字渲染(透明叠加到壁纸, alpha=0->露壁纸, 255->纯色)
void showDigit(int dx, int dy, const uint8_t* alphaMap, uint16_t pure, bool small) {
  TFT_eSprite* spr = small ? &numSprSmall : &numSpr;
  int w = spr->width(), h = spr->height();
  uint16_t* dp = (uint16_t*)spr->getPointer();
  uint16_t* wp = (uint16_t*)wallpaperSpr.getPointer();
  uint16_t pureSwapped = (pure >> 8) | (pure << 8);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      uint8_t a = pgm_read_byte(&alphaMap[y * w + x]);
      dp[y * w + x] = (a == 255) ? pureSwapped : wp[(dy + y) * 240 + (dx + x)];
    }
  }
  spr->pushSprite(dx, dy);
}

void showClockDigit(int x, int y, int n, char style) {
  if (style == 'W')      showDigit(x, y, A_W3660[n], COL_TEXT,   false);
  else if (style == 'O') showDigit(x, y, A_O3660[n], COL_ORANGE, false);
  else                   showDigit(x, y, A_W1830[n], COL_TEXT,   true);
}

void digitalClockDisplay() {
  getLocalTime(&timeinfo, 0);
  int h = timeinfo.tm_hour, m = timeinfo.tm_min, s = timeinfo.tm_sec, d = timeinfo.tm_mday;
  if (h != lastH) { showClockDigit(34, 58, h/10, 'W'); showClockDigit(70, 58, h%10, 'W'); lastH = h; }
  if (m != lastM) { showClockDigit(122, 58, m/10, 'O'); showClockDigit(158, 58, m%10, 'O'); lastM = m; }
  if (s != lastS) { showClockDigit(102, 128, s/10, 'w'); showClockDigit(120, 128, s%10, 'w'); lastS = s; }
  if (d != lastD) {
    wallpaperSpr.pushSprite(40, 166, 40, 166, 160, 24);   // 恢复日期区壁纸
    drawDate();
    lastD = d;
  }
}

void renderClock() {
  digitalClockDisplay();
}

// ============================================================
//  天气模式
// ============================================================
uint16_t tempColor(float t) {
  if (t < 0)   return 0x001F;
  if (t < 18)  return 0x07FF;
  if (t < 24)  return 0x07E0;
  if (t < 31)  return 0xFFE0;
  return 0xF800;
}
uint16_t aqiColor(int aqi) {
  if (aqi <= 50)  return 0x07E0;
  if (aqi <= 100) return 0xFFE0;
  if (aqi <= 150) return 0xFD20;
  if (aqi <= 200) return 0xF81F;
  return 0xF800;
}

bool fetchWeather() {
  char url[96];
  snprintf(url, sizeof(url), "http://d1.weather.com.cn/weather_index/%s.html?_=%ld",
           cityCode.c_str(), (long)millis());
  HTTPClient http;
  http.setTimeout(5000);
  http.begin(url);
  http.addHeader("Referer", "http://www.weather.com.cn/");
  http.setUserAgent("Mozilla/5.0 (iPhone; CPU iPhone OS 11_0 like Mac OS X)");
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String str = http.getString();
  http.end();
  int s = str.indexOf("dataSK =");
  int e = str.indexOf(";var dataZS");
  if (s < 0 || e < 0) return false;
  String jsonSK = str.substring(s + 8, e);
  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, jsonSK)) return false;
  w.temp  = doc["temp"].as<float>();
  w.humi  = atoi(doc["SD"].as<String>().c_str());
  w.press = doc["qy"].as<int>();
  w.aqi   = doc["aqi"].as<int>();
  String wc = doc["weathercode"].as<String>();
  // 修复：完整提取数字部分，不限位数（d2→2, d02→2, d301→301）
  w.code  = atoi(wc.substring(1).c_str());  // 去掉首字母 d/n，提取剩余完整数字
  w.ok = true;
  return true;
}

// ===== 树莓派 Hub 数据拉取（新增，优先使用，失败时回退上面的直连函数）=====

// ---- Hub 熔断器：补上 HALF-OPEN 状态 ----
// 原实现只有 CLOSED / OPEN 两态：piHubOffline 一旦置位就没有任何解除路径
// （成功分支里那两行 `piHubOffline = false` 被入口守卫挡住，是够不着的死代码），
// 结果是「只有重启设备才能恢复连 Pi」。
// 这里补上 HALF-OPEN：离线后每 PI_HUB_PROBE_MS 放行一次探测，成功即自动恢复。
// 这样无论因为什么原因闩上（Pi 重启 / WiFi 抖动 / 响应超时），系统都能自愈。
#define PI_HUB_PROBE_MS 60000UL

unsigned long piHubOfflineSince = 0;   // 进入离线态的时刻，用于半开计时（每次探测都刷新）
unsigned long piHubOfflineAt = 0;      // 真正【进入】离线态的时刻，只在迁移时写，用于量离线时长

// 「这一次到底有没有真的去试 Pi」——供包装函数区分两种返回 false 的情况：
//   ① 试了但失败          → 该打印「失败，回退直连」
//   ② 离线门控，压根没试   → 直连是【正常路径】，不该打印"失败"
// 两者都会让 fetchXxxFromPi 返回 false，不区分的话日志会撒谎（见 PERFORMANCE.md INC-004）。
static bool g_piAttempted = false;

static bool piHubAllowed() {
  g_piAttempted = false;                                // 默认：没试
  if (!usePiHub) return false;
  if (!piHubOffline) { g_piAttempted = true; return true; }   // CLOSED：正常放行
  // OPEN：每 PI_HUB_PROBE_MS 只放行一次，作为 HALF-OPEN 探测
  if ((long)(millis() - piHubOfflineSince) >= (long)PI_HUB_PROBE_MS) {
    piHubOfflineSince = millis();                       // 重置计时，避免连环探测
    Serial.println("[Pi] 探测 Hub 是否恢复...");
    g_piAttempted = true;
    return true;
  }
  return false;
}

// 统一的「标记离线」入口 —— 三处调用点共用，避免行为不一致
// ⚠️ 播报只在【状态真正迁移】(离线 → 离线 不算) 时发生。
//    早期写法把 Serial.println 放在 "failCount >= 3" 条件里，而跳闸后 failCount 不归零，
//    离线期间每次失败探测都让它 >= 3 → 重复打印。
//    后果：长跑日志里「标记离线」14 次 vs 「恢复在线」12 次，数字对不上，无法判断真实跳闸次数。
//    见 PERFORMANCE.md INC-006。
//
// 【若日后又看到「标记离线」次数异常，先按此判断是日志问题还是真问题】：
//    · 日志问题(已修) → 「标记离线」> 「恢复在线」，差值 = 离线期间的失败探测次数
//    · 真问题        → 「标记离线」与「恢复在线」数量匹配，但**绝对次数**异常增高
//                      （例如每几分钟一次），那说明 Pi 链路真的不稳，去查 WiFi/Flask/超时
// ⚠️ 这个宏必须在 piHubMarkOffline 之前 —— 那个函数里要用它。
// 含义见下方 piHubNoteTransportFail 的说明：只有【TCP 连接失败】才计入。
#define PI_HUB_FAIL_MAX 3

static void piHubMarkOffline(const char* src) {
  if (!piHubOffline) {
    Serial.printf("[Pi] %s 连续%u次【连接】失败，标记离线，回退直连\n", src, PI_HUB_FAIL_MAX);
    piHubOfflineAt = millis();   // ⚠️ 只在【状态迁移】时写，用来量真实离线时长。
                                 //    不能复用 piHubOfflineSince —— 那个每次探测都刷新，
                                 //    记的是"距上次探测多久"，不是"离线了多久"。
  }
  piHubOffline = true;
  piHubOfflineSince = millis();   // 计时每次都刷新：以最后一次尝试为基准重新计 60s
}

// ── Pi 存活判据（2026-09-14 重定义，依据见 PERFORMANCE.md 附录 10）──
// 【一句话】熔断器现在只反映「Pi 在不在」，不再混入"这次取数成不成功"。
//
// 为什么这么改（全部有实测支撑）：
//  ① 「取数失败 code=-11」= 读超时/卡死，与 Pi 生死【无关】：
//     全部日志 53 次失败 **100% 是 -11**，而 PC 侧同一时间窗口也有 3% 卡死。
//     更关键：把超时从 2s 抬到 5s，失败【数量一条没少】，耗时只是跟着阈值一起搬
//     （2032ms → 5042ms）—— 说明这批请求是"卡住"，不是"慢"。
//     ⇒ 拿它计入熔断器 = 拿噪声判生死。实测 14:54:13~48 的误跳闸就是这么来的。
//  ② 「TCP 连不上」才是真信号：83 分钟健康期 connect **零失败**（哪怕响应卡了 6 秒），
//     而两次掉线期分别出现 24 次 / 3 次。它不受超时长度影响 —— 超时判不出来，它能。
//  ③ 但它是【单边可靠】的："连得上"零假阴性；"连不上"有瞬时假阳性
//     （15:28:56 失败过一次，3 秒后就恢复）。所以用"连续 N 次"而不是一次。
//  ④ WiFi 没连上时【一律不计】：那是我自己没网。实测掉线期 connect 0~1ms 立即失败，
//     照单全收的话 15 秒内就会把"我没网"误判成"Pi 离线"。

// 三档指纹（实测值，用于把"连不上"归因到具体哪一层）
//   0~1 ms     = 无路由    → 多半是我自己没网（掉线期实测 0~1ms）
//   149~1313ms = 端口拒绝  → Pi 主机在，但服务没起（或绑到了别的地址）
//   2001~2002ms= 无应答    → Pi 主机不可达（断电 / 离线）
// ⚠️ 分界取 1700 而不是 1500：实测"端口拒绝"档抖动到过 1313ms（附录 13 ⑤），
//    1500 只剩 ~200ms 余量太薄。1700 与"无应答"档的 2001ms 仍留 ~300ms，两侧都够。
// ⚠️ 这只是【日志标签】，熔断器不依赖它 —— 别拿它当判据。
static const char* piConnectFailKind(unsigned long ms) {
  if (ms < 100)  return "无路由(自己没网?)";
  if (ms < 1700) return "端口拒绝(Pi主机在,服务没起?)";
  return "无应答(Pi主机不可达)";
}

// 「能不能连上 Pi」的错误码判定。
// ⚠️ 关键是【排除 -11 (HTTPC_ERROR_READ_TIMEOUT)】—— 实测它等于卡死，不是连不上。
static bool piCodeIsTransportFail(int code) {
  return code == -1    // CONNECTION_REFUSED  → Pi 在，端口拒绝
      || code == -4    // NOT_CONNECTED
      || code == -5    // CONNECTION_LOST
      || code == -7;   // NO_HTTP_SERVER
}

// Kline/Weather 走的是 http.begin(url)，没有单独的连接计时，
// 所以只能按 code 归因 —— 不要拿耗时档位去套（那是 Quote 路径才有的信息）。
static const char* piTransportCodeKind(int code) {
  switch (code) {
    case -1: return "端口拒绝(Pi主机在,服务没起?)";
    case -4: return "未连接";
    case -5: return "连接被断开";
    case -7: return "找不到服务";
    default: return "连接层失败";
  }
}

// 统一的「TCP 层失败」记账入口。返回 true 表示这一次真的计入了。
static bool piHubNoteTransportFail(const char* src, const char* why, unsigned long ms) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[Pi] %s 连接失败 [%s] —— WiFi 未连接，归因于【本机离线】，不计入熔断器\n",
                  src, why);
    return false;
  }
  piHubFailCount++;
  // ms==0 表示这条路径没有单独的连接计时（Kline/Weather），别打一个假的 0ms 出来
  if (ms) {
    Serial.printf("[Pi] %s 连接失败 耗时=%lums [%s] 连续第%lu次(阈值%u)\n",
                  src, ms, why, piHubFailCount, PI_HUB_FAIL_MAX);
  } else {
    Serial.printf("[Pi] %s 连接失败 [%s] 连续第%lu次(阈值%u)\n",
                  src, why, piHubFailCount, PI_HUB_FAIL_MAX);
  }
  if (piHubFailCount >= PI_HUB_FAIL_MAX) piHubMarkOffline(src);
  return true;
}

// 统一的「这次 Pi 取数成功了」入口（三个路径共用）。
// 成功即恢复 —— 这是【快路径】：TCP 连得上就说明 Pi 活着，不必等 HALF-OPEN 那一轮。
// 实测依据：83 分钟健康期 connect 零失败，"连得上"这个方向没有假阳性。
static void piHubNoteSuccess() {
  piHubFailCount = 0;
  if (piHubOffline) {
    piHubOffline = false;
    Serial.printf("[Pi] ✅ Hub 恢复在线（本次离线时长 %lus）\n",
                  (millis() - piHubOfflineAt) / 1000UL);
  }
}

// 从树莓派拉取行情+分时（合并接口，一次HTTP拿两份数据）
bool fetchQuoteFromPi(const char* symbol, Quote& q) {
  if (!piHubAllowed()) return false;

  char url[128];
  snprintf(url, sizeof(url), "http://%s:%d/api/stock/%s",
           piHubHost.c_str(), piHubPort, symbol);

  // ── 预连接：绕开 WiFiClient::connect(const char*) 里的 hostByName() ──
  // 读源码发现：即使 host 就是 IP 字面量，那条路也会走 hostByName()；
  // 而 hostByName() 里有 waitStatusBits(WIFI_DNS_IDLE_BIT, 16000) ——
  //   一个【进程级串行点】：任何一次 DNS 查询没结束，后面全部在这排队，最长等 16 秒。
  // connect(IPAddress, ...) 则直接 socket()+connect，一次 hostByName 都不调。
  // 既是修复尝试，也顺带把「纯连接耗时」单独量出来，与 HTTP 收发分开。
  // 见 PERFORMANCE.md 附录 4。⚠️ 这是【行为改动】，不是纯埋点。
  IPAddress piIp;
  WiFiClient piWc;
  unsigned long t0 = millis();
  bool preOk = piIp.fromString(piHubHost)
                 ? piWc.connect(piIp, (uint16_t)piHubPort, 2000)
                 : piWc.connect(piHubHost.c_str(), (uint16_t)piHubPort, 2000);
  unsigned long t1 = millis();
  piSumConn += (uint32_t)(t1 - t0);
  if ((uint32_t)(t1 - t0) > piMaxConn) piMaxConn = (uint32_t)(t1 - t0);

  if (!preOk) {                    // 连不上就不必再走 HTTP 了
    piWc.stop();
    unsigned long piMs = t1 - t0;
    piN++; piSum += piMs; if (piMs > piMax) piMax = piMs; if (piMs >= PI_SLOW_MS) piSlow++;
    piFail++;
    Serial.printf("[Pi] 预连接失败 耗时=%lums %s\n", piMs, symbol);
    // 预连接失败 = 纯 TCP 层失败 ⇒ 这是【唯一】该计入熔断器的一类
    piHubNoteTransportFail("Quote", piConnectFailKind(piMs), piMs);
    return false;
  }

  HTTPClient http;
  http.setTimeout(PI_HTTP_TIMEOUT_MS);  // ⚠️ 这是【读】超时；连接超时是独立的 _connectTimeout，默认 5000
  http.begin(piWc, url);  // 已连接 → HTTPClient 内部 connect() 直接 return true，不会重连
  int code = http.GET();
  unsigned long t2 = millis();
  piSumHttp += (uint32_t)(t2 - t1);
  if ((uint32_t)(t2 - t1) > piMaxHttp) piMaxHttp = (uint32_t)(t2 - t1);

  if (code != 200) {
    http.end();
    unsigned long t3 = millis();
    piSumEnd += (uint32_t)(t3 - t2);
    unsigned long piMs = t3 - t0;
    piN++; piSum += piMs; if (piMs > piMax) piMax = piMs; if (piMs >= PI_SLOW_MS) piSlow++;
    piFail++;
    Serial.printf("[Pi] 取数失败 code=%d 耗时=%lums (conn=%lu http=%lu) %s\n",
                  code, piMs, t1 - t0, t2 - t1, symbol);
    // ⚠️ 这里【故意不计入熔断器】。conn 段既然成功了，说明 Pi 是可达的；
    //    失败发生在读响应阶段（实测 100% 是 -11 读超时/卡死），那是"这次取数没成"，
    //    不是"Pi 不在了"。计入它就会用卡死噪声去判生死（见函数头部的说明）。
    //    本次失败照样会向上返回 false，由调用方回退到直连 —— 降级路径不受影响。
    if (piCodeIsTransportFail(code)) {   // 极少数情况：conn 成功但随后连接被重置
      piHubNoteTransportFail("Quote", "连接被重置", piMs);
    }
    return false;
  }

  String json = http.getString();
  unsigned long t3 = millis();
  http.end();
  unsigned long t4 = millis();
  piSumBody += (uint32_t)(t3 - t2);
  piSumEnd  += (uint32_t)(t4 - t3);
  if ((uint32_t)(t3 - t2) > piMaxBody) piMaxBody = (uint32_t)(t3 - t2);
  {
    unsigned long piMs = t4 - t0;
    // 这里只记【耗时】和【次数】。piOk/piFail 放在各自的 return 前加 ——
    // 否则解析失败/ok=false 的分支会被算成"成功"，统计就自相矛盾了。
    piN++; piSum += piMs; if (piMs > piMax) piMax = piMs; if (piMs >= PI_SLOW_MS) piSlow++;
    if (piMs >= PI_SLOW_MS) {
      // 这里是【实验 A 的数据来源】：阈值以上的样本会被截断成"失败"而丢掉耗时，
      // 所以"成功但慢"的这一批，正是唯一能看到延迟真实尾部的样本。
      // 分段必须一并打出来 —— 尾巴落在哪一段决定了该往哪儿优化。
      Serial.printf("[Pi] 慢请求 %lums %s  (conn=%lu http=%lu body=%lu end=%lu)\n",
                    piMs, symbol, t1 - t0, t2 - t1, t3 - t2, t4 - t3);
    }
  }

  StaticJsonDocument<2048> doc;  // 比原来的24KB小得多
  if (deserializeJson(doc, json)) {
    piFail++;   // 解析失败/ok=false：数据层问题，不动熔断器（Pi 是可达的）
    return false;
  }

  if (!doc["ok"].as<bool>()) {
    piFail++;   // 解析失败/ok=false：数据层问题，不动熔断器（Pi 是可达的）
    return false;
  }

  // 解析行情
  q.price     = doc["price"].as<float>();
  q.changePct = doc["pct"].as<float>();
  q.prevClose = doc["prev"].as<float>();
  q.high      = doc["high"].as<float>();
  q.low       = doc["low"].as<float>();
  q.change    = q.price - q.prevClose;
  q.ok = true;

  // 解析分时（48点固定长度）
  JsonArray sparkArr = doc["spark"];
  if (!sparkArr.isNull() && sparkArr.size() == 48) {
    q.sparkLen = 48;
    for (int i = 0; i < 48; i++) {
      q.spark[i] = sparkArr[i].as<float>();
    }
    q.sparkOk = true;
  }

  // 成功后重置失败计数
  piHubNoteSuccess();

  piOk++;          // ← 只有真正走到底才算成功（见上面 piN 那处的说明）
  return true;
}

// 从树莓派拉取日K
bool fetchKlineFromPi(const char* symbol, Quote& q) {
  if (!piHubAllowed()) return false;

  char url[128];
  snprintf(url, sizeof(url), "http://%s:%d/api/stock/%s/kline",
           piHubHost.c_str(), piHubPort, symbol);

  HTTPClient http;
  http.setTimeout(PI_HTTP_TIMEOUT_MS);
  http.begin(url);
  int code = http.GET();

  if (code != 200) {
    http.end();
    // 这条路径走的是 http.begin(url)（含 DNS + 连接），所以 code 能区分"连不上"和"读超时"：
    //   -11(READ_TIMEOUT) = 卡死，不计；-1/-4/-5/-7 = 连接层失败，计。
    if (piCodeIsTransportFail(code)) {
      piHubNoteTransportFail("Kline", piTransportCodeKind(code), 0);
    }
    return false;
  }

  String json = http.getString();
  http.end();

  StaticJsonDocument<3072> doc;  // 30组*4数*4字节≈480B + JSON开销
  if (deserializeJson(doc, json)) {
    // 数据层失败（解析不了 / ok=false / 字段缺失）：Pi 是可达的，不动熔断器
    return false;
  }

  if (!doc["ok"].as<bool>()) {
    // 数据层失败（解析不了 / ok=false / 字段缺失）：Pi 是可达的，不动熔断器
    return false;
  }

  JsonArray arr = doc["kl"];
  if (arr.isNull()) {
    // 数据层失败（解析不了 / ok=false / 字段缺失）：Pi 是可达的，不动熔断器
    return false;
  }

  q.klCount = 0;
  for (JsonArray candle : arr) {
    if (q.klCount >= KLINE_COUNT) break;
    if (candle.size() < 4) continue;
    q.kl[q.klCount][0] = candle[0].as<float>();  // 开
    q.kl[q.klCount][1] = candle[1].as<float>();  // 收
    q.kl[q.klCount][2] = candle[2].as<float>();  // 高
    q.kl[q.klCount][3] = candle[3].as<float>();  // 低
    q.klCount++;
  }
  q.klOk = q.klCount > 0;

  piHubNoteSuccess();

  return q.klOk;
}

// 从树莓派拉取天气
bool fetchWeatherFromPi() {
  if (!piHubAllowed()) return false;
  char url[128];
  snprintf(url, sizeof(url), "http://%s:%d/api/weather/%s",
           piHubHost.c_str(), piHubPort, cityCode.c_str());

  HTTPClient http;
  http.setTimeout(PI_HTTP_TIMEOUT_MS);
  http.begin(url);
  int code = http.GET();

  if (code != 200) {
    http.end();
    // 同 Kline：只有连接层错误码才算"连不上"，-11 读超时不计。
    if (piCodeIsTransportFail(code)) {
      piHubNoteTransportFail("Weather", piTransportCodeKind(code), 0);
    }
    return false;
  }

  String json = http.getString();
  http.end();

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, json)) {
    // 数据层失败（解析不了 / ok=false / 字段缺失）：Pi 是可达的，不动熔断器
    return false;
  }

  if (!doc["ok"].as<bool>()) {
    // 数据层失败（解析不了 / ok=false / 字段缺失）：Pi 是可达的，不动熔断器
    return false;
  }

  w.temp  = doc["temp"].as<float>();
  w.humi  = doc["humi"].as<int>();
  w.press = doc["press"].as<int>();
  w.aqi   = doc["aqi"].as<int>();
  w.code  = doc["code"].as<int>();
  w.ok = true;

  piHubNoteSuccess();

  return true;
}

// ===== 包装函数：优先Pi，失败回退直连 =====

bool fetchQuoteWrapper(const char* symbol, Quote& q) {
  if (fetchQuoteFromPi(symbol, q)) {
    Serial.printf("[Pi] Quote OK: %s %.2f (%.2f%%)\n", symbol, q.price, q.changePct);
    return true;
  }
  if (g_piAttempted) {                       // 只有真的试过 Pi 才叫"失败"
    Serial.printf("[Pi] Quote失败，回退直连: %s\n", symbol);
  }
  unsigned long tDc = millis();
  bool ok = fetchQuote(symbol, q);
  {
    // 直连兜底的真实代价 —— 这是"正反馈放大"假设的关键一环：
    // 兜底走 HTTPS+TLS，在单核 C3 上阻塞越久，就越容易把【下一个】Pi 请求顶过 2 秒。
    unsigned long dcMs = millis() - tDc;
    dcN++; dcSum += dcMs; if (dcMs > dcMax) dcMax = dcMs;
    Serial.printf("[Pi] 直连兜底 %s 耗时=%lums ok=%d\n", symbol, dcMs, (int)ok);
  }
  // 直连不提供分时数据(Pi 路径是同一个 JSON 一起返回的)，本该在这里补拉 —— 见 INC-003。
  // 但实测 fetchSpark **100% 失败**：
  //     [spark] sh000001 json err=InvalidInput strLen=12047
  // 能拿到 12KB 数据却不是合法 JSON，大概率和 httpsGet10() 的响应处理有关（待查）。
  // 而它每轮多发一次 HTTPS + TLS，在单核 C3 上代价很大 → 失败状态下是【纯亏】，先停用。
  // 实测影响：loop() 每轮耗时从 ~1.2s(Pi在线) 恶化到 ~6~9s(Pi离线)。
  // 【待办】打印 str 头部 120 字节定位 InvalidInput 后，再把下面这行恢复。
  // if (ok) fetchSpark(symbol, q);
  return ok;
}

bool fetchKlineWrapper(const char* symbol, Quote& q) {
  if (fetchKlineFromPi(symbol, q)) {
    Serial.printf("[Pi] Kline OK: %s %d根\n", symbol, q.klCount);
    return true;
  }
  if (g_piAttempted) {
    Serial.printf("[Pi] Kline失败，回退直连: %s\n", symbol);
  }
  return fetchKline(symbol, q);
}

bool fetchWeatherWrapper() {
  if (fetchWeatherFromPi()) {
    Serial.printf("[Pi] Weather OK: %.1f°C %d%%\n", w.temp, w.humi);
    return true;
  }
  if (g_piAttempted) {
    Serial.println("[Pi] Weather失败，回退直连");
  }
  return fetchWeather();
}


// 提取 ~ 分隔的第 n 个字段
String getField(String& data, int n) {
  int start = 0;
  for (int i = 0; i < n; i++) {
    int sep = data.indexOf('~', start);
    if (sep < 0) return "";
    start = sep + 1;
  }
  int sep = data.indexOf('~', start);
  if (sep < 0) return data.substring(start);
  return data.substring(start, sep);
}

// 抓行情(腾讯文本接口)
bool fetchQuote(const char* symbol, Quote& q) {
  char url[96];
  snprintf(url, sizeof(url), "http://qt.gtimg.cn/q=%s", symbol);
  HTTPClient http;
  http.setTimeout(5000);
  http.begin(url);
  http.setUserAgent("Mozilla/5.0");
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String str = http.getString();
  http.end();

  int s = str.indexOf('"');
  int e = str.lastIndexOf('"');
  if (s < 0 || e <= s) return false;
  String data = str.substring(s + 1, e);   // 1~贵州茅台~600519~1341.99~...

  q.price     = getField(data, 3).toFloat();
  q.prevClose = getField(data, 4).toFloat();
  q.change    = getField(data, 31).toFloat();
  q.changePct = getField(data, 32).toFloat();
  q.high      = getField(data, 33).toFloat();
  q.low       = getField(data, 34).toFloat();
  q.ok = true;
  return true;
}

// 抓日K(HTTPS, 前复权) -> kl[30][4]
bool fetchKline(const char* symbol, Quote& q) {
  char url[128];
  snprintf(url, sizeof(url), "https://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=%s,day,,,%d,qfq",
           symbol, KLINE_COUNT);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(8000);
  http.begin(client, url);
  http.addHeader("Referer", "http://gu.qq.com/");
  http.setUserAgent("Mozilla/5.0");
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String str = http.getString();
  http.end();
  client.stop();   // 释放 TLS 内存

  DynamicJsonDocument doc(24576);
  DeserializationError err = deserializeJson(doc, str);
  if (err) return false;

  JsonArray arr = doc["data"][symbol]["qfqday"];        // 股票: 前复权
  if (arr.isNull()) arr = doc["data"][symbol]["day"];   // 指数: 无复权用 day
  if (arr.isNull()) return false;

  q.klCount = 0;
  for (JsonVariant v : arr) {
    if (q.klCount >= KLINE_COUNT) break;
    q.kl[q.klCount][0] = v[1].as<float>();   // 开
    q.kl[q.klCount][1] = v[2].as<float>();   // 收
    q.kl[q.klCount][2] = v[3].as<float>();   // 高
    q.kl[q.klCount][3] = v[4].as<float>();   // 低
    q.klCount++;
  }
  q.klOk = q.klCount > 0;
  return q.klOk;
}

// 手动 HTTPS GET (HTTP/1.0 + Connection: close, 读到断开为止)
// HTTPClient 发 HTTP/1.1, 服务器回 chunked 分块; 其对 HTTPS+chunked 大响应(>4KB)会截断,
// 导致分时 JSON 不完整。改发 HTTP/1.0, 服务器回 Connection: close 不分块, 直接读到断开即可
bool httpsGet10(const String& url, String& out) {
  int hs = url.indexOf("://") + 3;
  int ps = url.indexOf('/', hs);
  String host = (ps < 0) ? url.substring(hs) : url.substring(hs, ps);
  String path = (ps < 0) ? "/" : url.substring(ps);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(8000);
  if (!client.connect(host.c_str(), 443)) return false;

  client.print("GET " + path + " HTTP/1.0\r\n");
  client.print("Host: " + host + "\r\n");
  client.print("User-Agent: Mozilla/5.0\r\n");
  client.print("Referer: http://gu.qq.com/\r\n");
  client.print("\r\n");

  // 跳过响应头(读到空行)
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line.length() <= 2) break;
  }

  // 读 body: 服务器 Connection: close, 读到断开; 关闭后多等片刻确认无残留 SSL 记录
  out = "";
  out.reserve(13000);
  uint8_t buf[256];
  unsigned long lastData = millis();
  while (millis() - lastData < 8000) {
    int avail = client.available();
    if (avail > 0) {
      int n = client.read(buf, (avail < (int)sizeof(buf)) ? avail : (int)sizeof(buf));
      if (n > 0) {
        out.concat((char*)buf, n);
        lastData = millis();
      }
    } else if (!client.connected()) {
      delay(20);                        // 等最后一段 SSL 记录解密
      if (client.available() == 0) break;
    }
    delay(1);
  }
  client.stop();

  // ── 【2026-09-17 balance-fix5】搬主线 INC-003 第一层的兜底（提交 20260915d）──────
  //   现象：上面那个"跳过响应头"的循环【并不可靠】—— 实测字符串里仍带着 HTTP 头，
  //         于是 deserializeJson 直接报 InvalidInput。
  //   为什么不去追根因：要追得烧一轮固件去试，而**这个兜底本来就该存在** ——
  //         找 JSON 起点这个判据【与原因无关】，两条路（头没跳净 / readStringUntil 超时返回空串）
  //         都能被它救。所以是有意的"不追根因，只兜底"。
  //   ⚠️ 不搬这一条，"直连补分时图"就是白改：fetchSpark 永远解析失败。
  int brace = out.indexOf('{');
  if (brace > 0) {
    Serial.printf("[https10] 头部未跳净，裁掉前 %d 字节\n", brace);
    out.remove(0, brace);
  }
  return out.length() > 0;
}

// 抓分时(HTTPS, HTTP/1.0 不分块) -> 均匀采样 spark[48]
bool fetchSpark(const char* symbol, Quote& q) {
  char url[128];
  snprintf(url, sizeof(url), "https://web.ifzq.gtimg.cn/appstock/app/minute/query?code=%s", symbol);

  String str;
  if (!httpsGet10(url, str)) {
    Serial.printf("[spark] %s httpGet fail len=%d\n", symbol, (int)str.length());
    return false;
  }

  DynamicJsonDocument doc(24576);
  DeserializationError err = deserializeJson(doc, str);
  if (err) {
    Serial.printf("[spark] %s json err=%s strLen=%d\n", symbol, err.c_str(), (int)str.length());
    return false;
  }

  JsonArray arr = doc["data"][symbol]["data"]["data"];
  if (arr.isNull()) {
    Serial.printf("[spark] %s arr null strLen=%d\n", symbol, (int)str.length());
    return false;
  }

  static float prices[241];   // 静态, 避免栈溢出
  int cnt = 0;
  for (JsonVariant v : arr) {
    if (cnt >= 241) break;
    String line = v.as<String>();   // "0930 1355.00 227 30758500.00"
    int sp1 = line.indexOf(' ');
    int sp2 = line.indexOf(' ', sp1 + 1);
    if (sp1 < 0 || sp2 < 0) continue;
    prices[cnt++] = line.substring(sp1 + 1, sp2).toFloat();
  }
  if (cnt < 2) {
    Serial.printf("[spark] %s cnt=%d\n", symbol, cnt);
    return false;
  }

  q.sparkLen = SPARK_POINTS;
  for (int i = 0; i < SPARK_POINTS; i++) {
    q.spark[i] = prices[(int)((long)i * cnt / SPARK_POINTS)];
  }
  q.sparkOk = true;
  return true;
}

void drawWeather() {
  char buf[24];
  if (!w.ok) {
    textSpr.fillSprite(TRANSPARENT);
    textSpr.setTextColor(COL_DIM);
    textSpr.setTextSize(2);
    textSpr.drawCentreString("No Weather Data", 80, 4, 2);
    textSpr.pushSprite(40, 100, TRANSPARENT);
    return;
  }

  // 天气图标(色键: 黑底透明)
  TJpgDec.setCallback(tft_output_icon);
  wrat.printfweather(90, 12, w.code);
  TJpgDec.setCallback(tft_output);

  // 温度大字(透明)
  snprintf(buf, sizeof(buf), "%.1f\140C", w.temp);
  tempSpr.fillSprite(TRANSPARENT);
  tempSpr.setTextFont(2);
  tempSpr.setTextSize(2);
  tempSpr.setTextColor(tempColor(w.temp));
  tempSpr.setCursor(0, 0);
  tempSpr.print(buf);
  int tw = tempSpr.textWidth(buf);
  tempSpr.pushSprite((240 - tw) / 2, 78, TRANSPARENT);

  // 城市名(透明)
  textSpr.fillSprite(TRANSPARENT);
  textSpr.setTextFont(1);
  textSpr.setTextSize(3);
  textSpr.setTextColor(COL_TEXT);
  textSpr.setCursor(0, 0);
  textSpr.print(cityLabel);
  int cw = textSpr.textWidth(cityLabel.c_str());
  textSpr.pushSprite((240 - cw) / 2, 116, TRANSPARENT);

  // 湿度图标(色键透明)
  TJpgDec.setCallback(tft_output_icon);
  TJpgDec.drawJpg(51, 168, humidity, sizeof(humidity));
  TJpgDec.setCallback(tft_output);

  // 湿度%(透明)
  snprintf(buf, sizeof(buf), "%d%%", w.humi);
  textSpr.fillSprite(TRANSPARENT);
  textSpr.setTextFont(2);
  textSpr.setTextSize(1);
  textSpr.setTextColor(COL_TEXT);
  textSpr.setCursor(0, 4);
  textSpr.print(buf);
  textSpr.pushSprite(79, 171, TRANSPARENT);

  // 气压(透明)
  snprintf(buf, sizeof(buf), "%dhPa", w.press);
  textSpr.fillSprite(TRANSPARENT);
  textSpr.setTextFont(2);
  textSpr.setTextSize(1);
  textSpr.setTextColor(COL_TEXT);
  textSpr.setCursor(0, 4);
  textSpr.print(buf);
  textSpr.pushSprite(129, 171, TRANSPARENT);

  // AQI 点(纯色圆点直接画) + 数值(透明)
  tft.fillCircle(95, 212, 8, aqiColor(w.aqi));
  snprintf(buf, sizeof(buf), "AQI %d", w.aqi);
  textSpr.fillSprite(TRANSPARENT);
  textSpr.setTextFont(2);
  textSpr.setTextSize(1);
  textSpr.setTextColor(COL_TEXT);
  textSpr.setCursor(0, 4);
  textSpr.print(buf);
  textSpr.pushSprite(108, 200, TRANSPARENT);
}

// ============================================================================
// 【2026-09-17 修复】天气节流闸门挡住了"本该无条件发生"的调用
// ----------------------------------------------------------------------------
// 病根（一句话）：
//   renderWeather() 的第一句是「时间没到就整个返回」—— 于是它不再是"渲染函数"，
//   而是一个【带闸门的动作】。而调用它的地方有两处想要"必须发生"（进天气模式、
//   切城市），却被这个闸门挡了回去。
//
// 两个发作点：
//   ① setMode() 天气分支：`w.ok = false; // 强制下次抓取` 紧跟 renderWeather();
//      ⇒ 成功取数一次后的 10 分钟内，整个函数当场返回
//      ⇒ 屏幕上只剩 setMode 画的那张壁纸，一个数据都没有 = 【仅壁纸无数据】
//   ② 切城市处理器：`w.ok = false;` + `pendingWeatherRefresh = true;`
//      ⇒ 同一个闸门 ⇒ 【城市切了，天气不刷新】
//
// ⚠️ 为什么【不能】照 New 版的 `if (!w.ok || 时间到)` 改回去：
//   把 !w.ok 放回条件里 ⇒ 取数失败时条件恒真 ⇒ 每轮 loop() 重试
//   ⇒ 每秒 20 次 TLS 握手 ⇒ CPU 饱和 ⇒ 整机失联 —— 那正是 INC-001 事故。
//   ⇒ 我们要的是"进入模式这一次必须发生"，不是"只要没数据就一直重试"。
//   ⇒ 所以用【一次性强制标志】，而不是把 !w.ok 放回条件。
//
// 改动代价：**零新增取数**（不增加任何 HTTP 请求）⇒ 不会破坏"切换流畅"。
//
// ⚠️ 三个变量【声明在文件顶部】的全局区（紧接着 `Weather w;`），不在这里 ——
//    因为 handleSet()（切城市，约 689 行）比本函数早得多，声明放这里会"先用后声明"编译不过。
// ============================================================================

void renderWeather() {
  // ⚠️ 这里踩过一个大坑，改之前先读完：
  // 旧写法是 `if (!w.ok || millis()-lastFetch >= 600000) { if(取数成功){ lastFetch=millis(); } }`
  // 两个错误叠加 —— ① lastFetch 只在成功时更新，失败时永远不前进；
  //                  ② 条件里的 !w.ok 让重试与时间彻底脱钩。
  // 结果：取数失败时条件恒为真，每轮 loop() 都重试(delay(50) → 每秒 20 次)，
  //       而失败路径是「回退直连」= HTTPS + TLS 握手(单核 C3 上 1~3 秒且吃满 CPU)。
  //       → 每秒 20 次 TLS 握手 → CPU 饱和 → AsyncTCP 被饿死 → 整机失联。
  // 教训：**节流条件里不要拿「数据是否就绪」当判据**，那会让重试频率与时间脱钩。
  //       失败必须退避，否则一次网络抖动就能把设备打进死亡螺旋。
  // 【2026-09-17】原为函数内 static，现提到文件作用域 —— 因为 setMode / 切城市
  //   需要能重置它或设置 g_weatherForce（见函数上方的完整说明）。
  // ⚠️ g_weatherForce 是【一次性】的：用完立刻清零，不会让"强制"变成"每次都不节流"。
  if (!g_weatherForce && (long)(millis() - g_weatherNextFetch) < 0) return;
  g_weatherForce = false;

  if (fetchWeatherWrapper()) {
    wallpaperSpr.pushSprite(0, 0);      // 恢复壁纸(清掉旧天气信息)
    drawWeather();
    g_weatherFailCount = 0;
    g_weatherNextFetch = millis() + 600000UL;    // 成功 → 10 分钟后刷新
  } else {
    // 失败退避 10s → 20s → 40s → 60s(封顶)
    // 重试频率从 20次/秒 降到最坏 1次/60秒，约 1200 倍
    // 封顶 60s 而非更长：抗风暴性质不变，但 Pi 恢复后最多等 1 分钟就自愈
    static const unsigned long WEATHER_BACKOFF_MS[4] = {10000UL, 20000UL, 40000UL, 60000UL};
    g_weatherNextFetch = millis() + WEATHER_BACKOFF_MS[g_weatherFailCount < 3 ? g_weatherFailCount : 3];
    if (g_weatherFailCount < 3) g_weatherFailCount++;
  }
}

// ============================================================
//  相册模式
// ============================================================
void drawPhoto(int idx) {
  if (SPIFFS.exists(PHOTOS[idx])) {
    TJpgDec.drawFsJpg(0, 0, PHOTOS[idx]);
  } else {
    tft.fillScreen(COL_BG);
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.drawCentreString("No Photo", 120, 110, 2);
  }
}

void renderPhoto() {
  if (millis() - lastRotate >= 5000UL) {
    int next = (curIdx + 1) % 3;
    drawPhoto(next);
    curIdx = next;
    lastRotate = millis();
  }
}

// ============================================================
//  股票模式(第一版布局: 多股轮动 + 分时/日K双视图)
// ============================================================

// 分时折线 sparkline
void drawSpark(const Quote& q, int x, int y, int w, int h, uint16_t col) {
  if (q.sparkLen < 2) return;
  float minV = q.spark[0], maxV = q.spark[0];
  for (int i = 1; i < q.sparkLen; i++) {
    if (q.spark[i] < minV) minV = q.spark[i];
    if (q.spark[i] > maxV) maxV = q.spark[i];
  }
  float range = maxV - minV;
  if (range < 0.001) range = 1;
  for (int i = 1; i < q.sparkLen; i++) {
    int x1 = x + (i - 1) * w / (q.sparkLen - 1);
    int y1 = y + h - (int)((q.spark[i - 1] - minV) / range * h);
    int x2 = x + i * w / (q.sparkLen - 1);
    int y2 = y + h - (int)((q.spark[i] - minV) / range * h);
    chartSpr.drawLine(x1, y1, x2, y2, col);
  }
}

// 日K蜡烛图
void drawKline(const Quote& q, int x, int y, int w, int h) {
  if (q.klCount < 2) return;
  float maxP = q.kl[0][2], minP = q.kl[0][3];
  for (int i = 1; i < q.klCount; i++) {
    if (q.kl[i][2] > maxP) maxP = q.kl[i][2];
    if (q.kl[i][3] < minP) minP = q.kl[i][3];
  }
  float range = maxP - minP;
  if (range < 0.001) range = 1;

  float barW = (float)w / q.klCount;
  int bodyW = (int)(barW * 0.7);
  if (bodyW < 1) bodyW = 1;

  for (int i = 0; i < q.klCount; i++) {
    uint16_t col = (q.kl[i][1] >= q.kl[i][0]) ? 0xF800 : 0x07E0;   // 红涨绿跌
    int yHigh  = y + (int)((maxP - q.kl[i][2]) / range * h);
    int yLow   = y + (int)((maxP - q.kl[i][3]) / range * h);
    int yOpen  = y + (int)((maxP - q.kl[i][0]) / range * h);
    int yClose = y + (int)((maxP - q.kl[i][1]) / range * h);
    int cx = x + (int)(i * barW + barW / 2);

    chartSpr.drawFastVLine(cx, yHigh, yLow - yHigh + 1, col);   // 影线
    int yTop = (yOpen < yClose) ? yOpen : yClose;          // 实体
    int yBot = (yOpen > yClose) ? yOpen : yClose;
    int bodyH = yBot - yTop;
    if (bodyH < 1) bodyH = 1;
    chartSpr.fillRect(cx - bodyW / 2, yTop, bodyW, bodyH, col);
  }
}

// 画单只股票(第一版布局)
// full=true:  全量重绘(切股/切视图/进入模式/数据可用状态变化)
// full=false: 增量刷新(只刷价格/涨跌幅/走势图, 文字不透明背景自覆盖, 走势图离屏原子推, 避免全屏闪烁)
void drawQuote(int idx, bool full) {
  const Sym&   s = SYMBOLS[idx];
  const Quote& q = quotes[idx];
  static bool lastOk[SYMBOL_COUNT] = {false, false, false, false};

  bool okChanged = (q.ok != lastOk[idx]);
  if (full || okChanged) tft.fillScreen(COL_BG);
  lastOk[idx] = q.ok;

  if (!q.ok) {
    if (full || okChanged) {
      tft.setTextColor(COL_DIM, COL_BG);
      tft.setTextSize(2);
      tft.setCursor(8, 100);
      tft.print(s.label);
      tft.setCursor(8, 130);
      tft.print("No Data");
    }
    return;
  }

  uint16_t col = (q.changePct > 0.01) ? 0xF800 :
                 (q.changePct < -0.01) ? 0x07E0 : 0xBDF7;

  // 名称 + 代码(静态, 仅全量时画)
  if (full || okChanged) {
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.setTextSize(2);
    tft.setCursor(8, 10);
    tft.print(s.label);

    tft.setTextColor(COL_DIM, COL_BG);
    tft.setTextSize(1);
    tft.setCursor(8, 215);
    tft.print(s.code);
  }

  // 价格(不透明背景自覆盖; 长度稳定, 无需清区)
  tft.setTextColor(col, COL_BG);
  tft.setTextSize(4);
  tft.setCursor(8, 45);
  tft.print(q.price, 2);

  // 涨跌幅(带符号, 正负等长自覆盖, 无需清区)
  tft.setTextSize(2);
  tft.setCursor(8, 95);
  if (q.changePct >= 0) tft.print("+");
  tft.print(q.changePct, 2);
  tft.print("%");

  // 走势图(离屏 sprite 原子推, 无闪烁)
  chartSpr.fillSprite(COL_BG);
  if (stockView == 0) drawSpark(q, 0, 0, 224, 80, col);
  else                drawKline(q, 0, 0, 224, 80);
  chartSpr.pushSprite(8, 125);
}

// 股票模式主循环(轮播 + 4股轮询刷新)
void renderStock() {
  unsigned long now = millis();

  // ⚠️⚠️ 【本课题铁律】一轮 loop() 最多 1 次取数。见 PERFORMANCE.md 附录20 §2。
  //   依据：ESP32→Pi 单次请求实测 1.5~1.8 秒。一轮两次 ⇒ 最坏单轮 3.6 秒 ⇒ 排队卡顿回归。
  //   这正是 5ad7843 → 2abda5d 那次回归的成因，也是用户凭手感确认过的分界线。
  //   ⇒ 任何新增的取数都必须先检查 didFetch，绝不能直接往轮播块里加。
  bool didFetch = false;

  // 轮播: 5 秒翻页
  // ⚠️ 这里【只画，不取数】—— 5ad7843 的结构，务必保持。往这里加取数 = 一轮两次 = 卡顿回归。
  if (now - stockLastRotate >= 5000UL) {
    stockCurIdx = (stockCurIdx + 1) % SYMBOL_COUNT;
    drawQuote(stockCurIdx, true);   // 切股: 全量
    stockLastRotate = now;
  }

  // ── 日K按需：独立闸门（附录20）────────────────────────────────────────────
  //   ① 只给【当前显示的那一只】拉 —— 正是马上要画的那只
  //   ② 自带 TTL（每股一份戳）：日K一天才出一根 ⇒ TTL 给长毫无损失
  //   ③ 受 didFetch 互斥 ⇒ 一轮最多一次
  //
  //   【历史】原判据是 `stockView == 1 && refreshIdx == stockCurIdx`，
  //     在 refreshInterval=20 时【结构性恒假】（详见 klineStamp 处的说明）
  //     ⇒ 日K 一次都没拉过 ⇒ 日K图永远空白。
  if (stockView == 1) {
    int i = stockCurIdx;
    if (klineStamp[i] == 0 || now - klineStamp[i] >= KLINE_TTL_MS) {
      if (!didFetch) {
        fetchKlineWrapper(SYMBOLS[i].code, quotes[i]);
        klineStamp[i] = millis();
        didFetch = true;
      }
    }
  }

  // ── 直连补分时图（Pi 离线时才做）【balance-fix5，附录22】────────────────────
  //   ① **判据用 `piHubOffline`**：Pi 在线时 spark 随 quote 免费回来 ⇒ **绝不能重复拉**。
  //      滞后窗口（掉线后前 3 次 TCP 失败）期间显示上一次的数据 —— 旧但不空，可接受。
  //   ② 只给【当前显示的那一只】拉
  //   ③ 每股一份戳 + TTL：全局戳会退化成"只有一只是新的"（与 INC-003/附录20 同类的共振错误）
  //   ④ **尝试即推进**：失败也等 TTL，否则失败 ⇒ 每轮重试 ⇒ 重试风暴
  //   ⑤ 受 didFetch 互斥 ⇒ 一轮最多一次取数（铁律）
  if (DIRECT_SPARK_BACKFILL && stockView == 0 && piHubOffline) {
    int i = stockCurIdx;
    if (sparkStamp[i] == 0 || now - sparkStamp[i] >= SPARK_TTL_MS) {
      if (!didFetch) {
        fetchSpark(SYMBOLS[i].code, quotes[i]);
        sparkStamp[i] = millis();
        didFetch = true;
      }
    }
  }

  // 4股轮询刷新(股票延迟敏感，需要持续更新所有自选股)
  // 每 refreshInterval/SYMBOL_COUNT 秒刷一只: 行情+分时(同一个请求一起回来)
  static unsigned long lastRefresh = 0;
  if (now - lastRefresh >= (unsigned long)refreshInterval * 1000 / SYMBOL_COUNT) {
    static int refreshIdx = 0;
    // ⚠️ 加 !didFetch 互斥：本轮若已为日K取过数，这次就跳过（保铁律）。
    //    代价是那一格行情晚 1 个周期（refreshInterval 秒）—— 与"卡顿回归"相比不值一提。
    if (!didFetch) {
      fetchQuoteWrapper(SYMBOLS[refreshIdx].code, quotes[refreshIdx]);
      didFetch = true;
    }
    // 分时已在 fetchQuoteWrapper 里一起拉了，不再单独调 fetchSpark
    if (refreshIdx == stockCurIdx) drawQuote(stockCurIdx, false);   // 刷当前股: 增量
    refreshIdx = (refreshIdx + 1) % SYMBOL_COUNT;
    lastRefresh = now;
  }
}

// ============================================================
//  模式分发
// ============================================================
void renderCurrentMode() {
  switch (currentMode) {
    case MODE_CLOCK:   renderClock();   break;
    case MODE_WEATHER: renderWeather(); break;
    case MODE_PHOTO:   renderPhoto();   break;
    case MODE_STOCK:   renderStock();   break;
  }
}

void setMode(Mode m) {
  currentMode = m;
  prefs.putInt("defaultMode", m);
  tft.fillScreen(TFT_BLACK);
  // 模式进入初始化
  switch (m) {
    case MODE_CLOCK:
      lastH = lastM = lastS = lastD = -1;   // 重置节流, 强制全量重绘
      if (!chartSprFreed) { chartSpr.deleteSprite(); chartSprFreed = true; }   // 腾 35KB
      freeSmallSprites();                   // 腾 32KB(小 sprite 可能没释放)
      ensureWallpaper();                    // 先重建大块壁纸(此时堆最充裕)
      ensureSmallSprites();                 // 再重建小 sprite
      showWallpaper();                      // 根据壁纸模式显示背景
      drawColon();
      digitalClockDisplay();
      break;
    case MODE_WEATHER:
      // 【2026-09-17 修复「仅有壁纸没有数据」】
      //   原来这里是 `w.ok = false; // 强制下次抓取` + `renderWeather();` ——
      //   意图对，但**做不到**：renderWeather() 第一句就是节流闸门，
      //   成功取数一次后的 10 分钟内整个函数当场返回
      //   ⇒ 数据一字不画，屏幕上只剩下面 showWallpaper() 画的那张壁纸。
      //
      //   现在分两条路，各自对症（见 renderWeather 上方的完整说明）：
      //     有缓存(w.ok) ⇒ 【立即画缓存】= 0 阻塞 ⇒ 切换瞬时，数据不缺失
      //     没缓存       ⇒ 没东西可画，只能当场取（唯一会阻塞的场合）
      //   ⚠️ 这里【不置】w.ok = false：旧数据仍然有效（最多陈旧 10 分钟，
      //      与本版原有的 TTL 一致），先画出来远好过"什么都不画"。
      if (!chartSprFreed) { chartSpr.deleteSprite(); chartSprFreed = true; }
      freeSmallSprites();
      ensureWallpaper();         // 先重建大块壁纸
      ensureSmallSprites();      // 再重建小 sprite
      showWallpaper();           // 根据壁纸模式显示背景
      if (w.ok) {
        drawWeather();           // 立即叠上上一次的数据（0 阻塞）
      } else {
        g_weatherForce = true;   // 从没取到过 ⇒ 无条件取一次
        renderWeather();
      }
      break;
    case MODE_PHOTO:
      drawPhoto(curIdx);
      lastRotate = millis();
      break;
    case MODE_STOCK:
      stockCurIdx = 0;
      stockLastRotate = millis();
      if (!wallpaperFreed) { wallpaperSpr.deleteSprite(); wallpaperFreed = true; }   // 腾 115KB 给 HTTPS
      freeSmallSprites();        // 股票不用小 sprite, 再腾 32KB
      ensureChart();             // 重建图表缓冲(可能被时钟/天气释放)
      drawQuote(0, true);
      break;
  }
}

// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  // ⚠️ 埋点必须放在 Serial.begin() 之后。
  //    上一版把 printf 写在了 Serial.begin() 前面 —— 串口还没初始化，输出全部被丢弃，
  //    结果 9 次重启一条 [BOOT] 都没打出来，白白烧了一轮固件。见 PERFORMANCE.md INC-008。
  {
    esp_reset_reason_t rr = esp_reset_reason();
    const char* nm = (rr == ESP_RST_POWERON) ? "上电/POWERON"
                   : (rr == ESP_RST_SW)      ? "软件重启/SW(有代码调了 ESP.restart)"
                   : (rr == ESP_RST_PANIC)   ? "崩溃/PANIC(异常/abort)"
                   : (rr == ESP_RST_INT_WDT) ? "中断看门狗/INT_WDT"
                   : (rr == ESP_RST_TASK_WDT)? "任务看门狗/TASK_WDT(loop 阻塞>5s)"
                   : (rr == ESP_RST_WDT)     ? "其他看门狗/WDT"
                   : (rr == ESP_RST_BROWNOUT)? "掉电/BROWNOUT(供电不足)"
                   : "其他";
    if (g_rtcMagic != RTC_MAGIC) {          // 冷启动/掉电：RTC 内容不可信，重新计数
      g_rtcMagic = RTC_MAGIC; g_rtcBoots = 0; g_rtcPanicBoots = 0;
      g_rtcStage = 0; g_rtcStageFree = 0; g_rtcStageMax = 0;
    }
    g_rtcBoots++;
    g_rtcLastReset = (uint32_t)rr;
    if (rr == ESP_RST_PANIC) g_rtcPanicBoots++;   // 只把真崩溃计入
    Serial.printf("[BOOT] 第%lu次启动 复位原因=%d (%s) 累计崩溃=%lu\n",
                  (unsigned long)g_rtcBoots, (int)rr, nm,
                  (unsigned long)g_rtcPanicBoots);
    Serial.printf("[BOOT] 上次死前: loop阶段=%lu free=%lu maxAlloc=%lu\n",
                  (unsigned long)g_rtcStage, (unsigned long)g_rtcStageFree,
                  (unsigned long)g_rtcStageMax);
    serialDrain(40);                         // 不是 Serial.flush()！见文件顶部说明
    g_rtcStage = 0;                          // 清掉，避免下次误读成"死在阶段0"
  }

  Serial.println("\n=== SDD 小电视 整合版 (Step2) ===");
  // 【2026-09-17 加】固件标识 —— 否则无法确认设备上跑的是哪一版。
  //   本项目为此吃过亏，主线后来专门加了 FW_TAG（提交 0cc9c36）；
  //   平衡版分支原本没有，这里补一个最小版本（只打印，不改任何行为）。
  Serial.printf("[FW] %s\n", FW_TAG);

  setBacklight();
  tft.init();
  tft.setRotation(0);
  tft.invertDisplay(true);
  tft.fillScreen(TFT_BLACK);

  prefs.begin("sdd", false);
  loadConfig();
  setBrightness(brightness);  // 应用从NVS加载的亮度

  // 配网
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawCentreString("配网中...", 120, 100, 2);
  setupWifi();

  // SPIFFS(相册)
  if (!SPIFFS.begin(true)) Serial.println("SPIFFS 挂载失败");
  cleanupSPIFFS();  // 清理无用文件，保持存储干净

  // JPEG 解码器
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tft_output);

  // NTP(时钟)
  configTime(8 * 3600, 0, "ntp.aliyun.com", "ntp6.aliyun.com");
  getLocalTime(&timeinfo, 10000);

  // 开机自动亮度检测：若已开启自动调节，立即按当前时段刷新亮度
  // 避免重启后停留在旧亮度直到下次60秒轮询才生效
  if (autoBrightness) {
    autoAdjustBrightness();
  }

  // AsyncWebServer(切模式+壁纸设置+WiFi管理+城市切换)
  server.on("/", AsyncWebRequestMethod::HTTP_GET, handleRoot);
  server.on("/set", AsyncWebRequestMethod::HTTP_GET, handleSet);
  server.on("/wp_select", AsyncWebRequestMethod::HTTP_GET, handleWallpaperSelect);
  server.on("/m.jpg", AsyncWebRequestMethod::HTTP_GET, handleImage);
  server.on("/city_data.json", AsyncWebRequestMethod::HTTP_GET, handleCityDataJSON);
  server.on("/spiffs_list", AsyncWebRequestMethod::HTTP_GET, handleSPIFFSList);  // 临时调试：查看SPIFFS文件列表

  // /status —— 给压测脚本用的自检端点（见 PERFORMANCE.md INC-008）
  // 为什么需要它：判断"设备到底有没有重启"以前只能靠人肉盯串口，
  // 而串口在 CDC 下会【积压/丢行/粘行】（实测：启动日志被扣了 2 分 41 秒才吐出来），
  // 已经两次导致误判。这里把 RTC 里的累计启动次数直接暴露成 JSON，脚本一读就知道。
  // 用 lambda 而不是具名函数：避免 Arduino 自动原型插入点被顶到类型定义之前。
  server.on("/status", AsyncWebRequestMethod::HTTP_GET,
    [](AsyncWebServerRequest *request) {
      char buf[288];
      snprintf(buf, sizeof(buf),
        "{\"boots\":%lu,\"panicBoots\":%lu,\"lastReset\":%lu,\"uptime\":%lu,"
        "\"free\":%u,\"maxAlloc\":%u,"
        "\"rssi\":%d,\"mode\":%d,\"piOffline\":%s,\"chartReady\":%s}",
        (unsigned long)g_rtcBoots,
        (unsigned long)g_rtcPanicBoots,     // ← 只数真崩溃，压测脚本靠它判定
        (unsigned long)g_rtcLastReset,
        (unsigned long)(millis() / 1000UL),
        (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
        WiFi.RSSI(), (int)currentMode,
        piHubOffline ? "true" : "false",
        chartSprFreed ? "false" : "true");
      request->send(200, "application/json", buf);
    });
  server.on("/upload", AsyncWebRequestMethod::HTTP_GET, handleUploadPage);
  server.on("/stock_edit", AsyncWebRequestMethod::HTTP_GET, handleStockEdit);
  server.on("/stock_save", AsyncWebRequestMethod::HTTP_GET, handleStockSave);
  server.on("/change_wifi", AsyncWebRequestMethod::HTTP_GET, handleChangeWiFi);
  server.on("/do_change_wifi", AsyncWebRequestMethod::HTTP_GET, handleDoChangeWiFi);
  server.on("/reset_wifi", AsyncWebRequestMethod::HTTP_GET, handleResetWiFi);
  server.on("/city_list", AsyncWebRequestMethod::HTTP_GET, handleCityList);
  server.on("/do_upload", AsyncWebRequestMethod::HTTP_POST,
    // 请求完成回调：检查全局标志，成功才重启
    [](AsyncWebServerRequest *request) {
      if (uploadSuccess) {
        request->send(200, "text/html; charset=utf-8",
          "<html><body style='font-family:sans-serif;background:#1a1a2e;color:#eee;text-align:center;padding:40px'>"
          "<h3>上传完成，设备将在 2 秒后重启</h3></body></html>");
        pendingRestartAt = millis() + 2000;   // 异步延迟重启，不阻塞响应发送
      } else {
        request->send(400, "text/html; charset=utf-8",
          "<html><body style='font-family:sans-serif;background:#1a1a2e;color:#eee;text-align:center;padding:40px'>"
          "<h3 style='color:#f87171'>上传失败: " + uploadError + "</h3>"
          "<p><a href='/upload' style='color:#38bdf8'>返回重试</a></p></body></html>");
      }
    },
    // 分块上传回调：白名单验证 + 大小限制
    [](AsyncWebServerRequest *request, String filename, size_t index,
       uint8_t *data, size_t len, bool final) {
      if (index == 0) {   // 首块：初始化
        uploadSuccess = false;
        uploadError = "";
        uploadedBytes = 0;

        String targetName = request->hasParam("fname", true)
                          ? request->getParam("fname", true)->value() : filename;

        // 白名单验证（只允许指定文件名）
        const char* ALLOWED_FILES[] = {
          "m.jpg",                     // 网页壁纸
          "1.jpg", "2.jpg", "3.jpg",   // 相册
          "city_data.json"             // 城市数据
        };
        bool valid = false;
        for (int i = 0; i < (int)(sizeof(ALLOWED_FILES) / sizeof(ALLOWED_FILES[0])); i++) {
          if (targetName == ALLOWED_FILES[i]) { valid = true; break; }
        }
        if (!valid) {
          uploadError = "文件名不在白名单: " + targetName;
          Serial.println("拒绝上传: " + uploadError);
          return;
        }

        if (fsUploadFile) fsUploadFile.close();
        fsUploadFile = SPIFFS.open("/" + targetName, "w");
        if (!fsUploadFile) {
          uploadError = "无法创建文件: " + targetName;
          Serial.println("上传失败: " + uploadError);
          return;
        }
        Serial.printf("开始上传: %s\n", targetName.c_str());
      }

      if (uploadError.length() > 0) return;   // 已拒绝，跳过写入

      // 大小限制（单个文件最大500KB，按累计字节判断）
      uploadedBytes += len;
      if (uploadedBytes > 512000) {
        uploadError = "文件过大(最大500KB)";
        Serial.println("拒绝上传: " + uploadError);
        if (fsUploadFile) fsUploadFile.close();
        return;
      }

      if (fsUploadFile && len > 0) {
        size_t written = fsUploadFile.write(data, len);
        if (written != len) {
          uploadError = "写入失败";
          Serial.println("上传失败: " + uploadError);
          fsUploadFile.close();
          return;
        }
      }

      if (final) {
        if (fsUploadFile) {
          fsUploadFile.close();
          Serial.println("上传成功");
          uploadSuccess = true;
        } else {
          uploadError = "文件句柄无效";
        }
      }
    });
  // 诊断：记录 server.begin() 前后的堆状态
  // 关键：AsyncTCP 要起一个 16KB 栈的后台任务，必须在 sprite 占堆之前调用
  uint32_t heapBefore = ESP.getFreeHeap();
  Serial.printf("[诊断] server.begin() 前堆: %u 字节可用\n", heapBefore);
  server.begin();
  uint32_t heapAfter = ESP.getFreeHeap();
  Serial.printf("[诊断] server.begin() 后堆: %u 字节可用 (消耗 %d)\n",
                heapAfter, (int)heapBefore - (int)heapAfter);
  if ((int)heapBefore - (int)heapAfter < 8000) {
    Serial.println("[诊断] ✗ AsyncTCP 任务疑似创建失败(消耗<8KB)，Web 将无法访问！");
  } else {
    Serial.println("[诊断] ✓ AsyncTCP 后台任务已启动");
  }

  // sprite(必须在 server.begin() 之后创建，否则挤掉 AsyncTCP 的任务栈)
  numSpr.setColorDepth(16);
  numSpr.createSprite(36, 60);
  numSprSmall.setColorDepth(16);
  numSprSmall.createSprite(18, 30);
  wallpaperSpr.setColorDepth(16);
  wallpaperSpr.createSprite(240, 240);
  dateSpr.setColorDepth(16);
  dateSpr.createSprite(120, 24);
  tempSpr.setColorDepth(16);
  tempSpr.createSprite(160, 40);
  textSpr.setColorDepth(16);
  textSpr.createSprite(160, 24);
  // chartSpr 是第 7 个创建，此前 wallpaperSpr 已吃掉 115KB，堆已碎片化 ——
  // 实测它【确实创建失败】(启动日志显示 7 个里只成功了 6 个)。
  // 关键：失败时必须把 chartSprFreed 置 true，否则 ensureChart() 会认为
  // "不需要重建"而永不重试 —— 后果是股票图表一直空白，**切一次模式才恢复**
  // （因为切到时钟/天气会释放 chartSpr 并把该标志置 true）。见 PERFORMANCE.md INC-005。
  chartSpr.setColorDepth(16);
  if (chartSpr.createSprite(224, 80) == nullptr) {
    chartSprFreed = true;   // 标记"未就绪" → 进股票模式时 ensureChart() 会重建
    Serial.printf("[chart] 启动创建失败 (free=%u maxAlloc=%u)，将在进入股票模式时重建\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }
  // 验证 server 是否真的监听了（AsyncTCP 失败是静默的）
  // 简单检查：如果 begin() 真成功了，至少不会立即崩溃
  delay(100);  // 给异步任务一点启动时间
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[自检] WiFi 已连接: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[自检] 网关: %s, DNS: %s\n",
                  WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str());
    Serial.println("[自检] 浏览器访问 http://" + WiFi.localIP().toString() + "/");
  }
  // 【2026-09-15 清理】原先这里还有一段"启动时探测 Pi Hub"的自检，已删除。三个理由：
  //   ① 它把 Pi 的地址【又写死了一份】—— 那是固件里【第二份 Pi 地址真值】，
  //      绕开了 piHubHost。换网络时改了一个改不到另一个 ⇒ 这份自检会永远报
  //      "不可达"，反而【误导诊断】。
  //   ② 启动时多一次阻塞 HTTP（超时 3 秒），单核 C3 上白等。
  //   ③ 它探的 /health 端点，原注释自己都写着"如果有" —— 本就不确定存在。
  //   运行期的 [Pi] 日志已经完整覆盖这个信息（真实取数 + TCP 三档指纹归因），自检没有增量价值。
  //
  // 【保留】上面三条 [自检] 打印（IP/网关/DNS/访问地址）—— 配网和排查时要看。
  // 【保留】前面 `[诊断] server.begin() 前后堆` 那一段 —— 见 esp32-asyncwebserver-heap-order：
  //   begin() 失败是【静默】的，堆差是唯一哨兵，删了就没有任何办法发现 Web 服务没起来。

  // 从 NVS 加载股票代码+名称（没有就用默认值）
  for (int i = 0; i < SYMBOL_COUNT; i++) {
    String keyC = "stock" + String(i);
    String keyL = "label" + String(i);
    String savedC = prefs.getString(keyC.c_str(), "");
    String savedL = prefs.getString(keyL.c_str(), "");
    if (savedC.length() > 0) {
      static char bufC[4][16];  // 静态缓冲区（全局生命周期），否则指针失效
      static char bufL[4][16];
      savedC.toCharArray(bufC[i], 16);
      SYMBOLS[i].code = bufC[i];
      if (savedL.length() > 0) {
        savedL.toCharArray(bufL[i], 16);
        SYMBOLS[i].label = bufL[i];
      }
      Serial.printf("股票%d: %s / %s (从 NVS 加载)\n", i + 1, bufC[i], SYMBOLS[i].label);
    }
  }

  // 进入默认模式
  currentMode = (Mode)defaultMode;
  setMode(currentMode);

  Serial.println("WebServer: http://" + WiFi.localIP().toString());
}

void loop() {
  // AsyncWebServer 无需 handleClient()，请求在后台任务中处理
  // 以下 pending 任务由 Web 回调置标志，在这里执行，避免阻塞 HTTP 响应

  static uint32_t loopCount = 0;
  loopCount++;

  // INC-008 面包屑：把本轮起点状态写进 RTC —— 复位后由 [BOOT] 打印出来。
  // 阶段编号：1=loop起点 10=pendingModeChange→setMode 内 11=setMode 返回
  //           20=renderCurrentMode 内 30=loop末尾
  g_rtcStage = 1; g_rtcStageFree = ESP.getFreeHeap(); g_rtcStageMax = ESP.getMaxAllocHeap();

  // WiFi 状态变化记录 + 掉线恢复
  //
  // 背景：全固件此前【完全没有】掉线监测与恢复（只有 setup 里查过一次 status）。
  // 实测铁证（logs/pi_rtt_0916.log）：13:47 掉线，之后【23 分钟零恢复、零重启】——
  //   固件一直"正常运行"，只是永远没有网络。生产环境里热点抖一下设备就永久失联，
  //   必须人工重启。这比延迟问题严重得多。
  // 恢复策略：30s 重试一次；连续 3 次（≈1.5 分钟）仍失败 → 重启，比重刷固件体面。
  //   注意重启是 ESP_RST_SW，不会计入 panicBoots，压测脚本能区分开。
  //
  // 【第二轮修正 · 2026-09-14】重试手段从 WiFi.reconnect() 换成"重新关联"。
  //   实测铁证（logs/pi_warm_0916.log 15:30:32~15:32:34）：掉线后 6 次 WiFi.reconnect()
  //   【全部失败】，最后由重启兜底才恢复在线 —— 也就是说原来的快路径是零收益的。
  //   reconnect() 只让驱动重发 connect；若 AP 侧还持有旧关联状态，就会一直卡着。
  //   换成：读 NVS 凭据 → WiFi.disconnect(不关射频/不清凭据) → WiFi.begin(ssid, psk)，
  //   即完整重走一次关联握手。阈值同时从 6 次缩到 3 次，把最长失联从 3 分钟压到 1.5 分钟。
  {
    static wl_status_t lastWifiSt = WL_IDLE_STATUS;
    static bool wifiStInit = false;
    static unsigned long lastWifiRetry = 0;
    static unsigned long wifiDownSince = 0;   // 掉线起始时刻；0 = 当前在线
    static uint8_t wifiRetryCount = 0;
    wl_status_t ws = WiFi.status();

    if (!wifiStInit) { lastWifiSt = ws; wifiStInit = true; }
    else if (ws != lastWifiSt) {
      Serial.printf("[WiFi] 状态变化 %d -> %d (%s) uptime=%lus free=%u\n",
                    (int)lastWifiSt, (int)ws,
                    (ws == WL_CONNECTED) ? "已连接" : "已断开",
                    millis() / 1000UL, (unsigned)ESP.getFreeHeap());
      lastWifiSt = ws;
      if (ws != WL_CONNECTED) lastWifiRetry = 0;   // 刚掉线：立即允许第一次重试
    }

    if (ws != WL_CONNECTED) {
      if (wifiDownSince == 0) wifiDownSince = millis();
      if (millis() - lastWifiRetry >= 30000UL) {
        lastWifiRetry = millis();
        wifiRetryCount++;
        unsigned long downS = (millis() - wifiDownSince) / 1000UL;
        if (wifiRetryCount > 3) {
          Serial.printf("[WiFi] 连续 3 次重连失败(已失联%lus)，重启设备\n", downS);
          serialDrain(40);
          ESP.restart();
        }

        // ⚠️ 凭据必须【先读出来】再 disconnect —— WiFi.SSID() 走的是
        //    esp_wifi_sta_get_ap_info()，只在【已连接】时有值；掉线时它返回空串。
        //    所以这里直接读驱动里存的 STA 配置(断不断线都读得到)。
        //    ssid[32]/password[64] 不保证带结束符，按字段宽度拷进 0 初始化的缓冲区。
        char sid[33] = {0};
        char pwd[65] = {0};
        wifi_config_t conf;
        bool haveCfg = (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK);
        if (haveCfg) {
          memcpy(sid, conf.sta.ssid, 32);
          memcpy(pwd, conf.sta.password, 64);
        }
        Serial.printf("[WiFi] 掉线%lus(状态=%d) 第%u次重连 rssi=%d free=%u ssid=%s\n",
                      downS, (int)ws, wifiRetryCount, WiFi.RSSI(),
                      (unsigned)ESP.getFreeHeap(), haveCfg ? sid : "(读不到配置)");
        WiFi.disconnect(false, false);   // 不关射频、不清凭据，只断当前关联
        delay(100);
        if (haveCfg && sid[0]) WiFi.begin(sid, pwd);  // 完整重走一次关联
        else                   WiFi.begin();          // 万不得已：用驱动内已有配置
      }
    } else {
      if (wifiRetryCount) {
        Serial.printf("[WiFi] ✅ 重连成功，共尝试 %u 次 失联时长=%lus\n",
                      wifiRetryCount, (millis() - wifiDownSince) / 1000UL);
        wifiRetryCount = 0;
      }
      wifiDownSince = 0;
    }
  }
  static unsigned long lastSeenPending = 0;
  if (pendingRestartAt != lastSeenPending) {
    lastSeenPending = pendingRestartAt;
    if (pendingRestartAt != 0) {
      // 正常路径下这里只会在"保存股票/重置WiFi/上传完成"后出现一次；
      // 压测里如果反复出现 = 变量被写坏(内存越界)，而不是入口被命中。
      Serial.printf("[RST?] pendingRestartAt 被置为 %lu (now=%lu, 还有 %ld ms)\n",
                    pendingRestartAt, millis(), (long)(pendingRestartAt - millis()));
      serialDrain(20);
    }
  }

  // 长跑观测心跳(每 60s 一条)：
  //   loopMs        【CPU 压力指标：每轮 loop() 的平均耗时(毫秒)】
  //   free 缓慢下降  = 内存泄漏
  //   maxAlloc 下降而 free 平稳 = 堆碎片化(sprite 重建会失败)
  //   asyncStackFree 剩余 < 2KB = 回调栈危险
  //
  // loopMs 读法：loop() 末尾有 delay(50)，所以【健康值 ≈ 50 ms】。
  //   ≈ 50   健康，没有额外阻塞
  //   数百   每轮被额外占用，差值就是阻塞的代价
  //   上千   有重量级阻塞（HTTPS + TLS 握手，单核 C3 上 1~3 秒/次）
  //
  // ⚠️ 为什么不用 loopHz：整数除法把 1Hz 以下全截成 0。
  //    实测 A 组(Pi在线)和 B 组(Pi离线)都显示 loopHz=0，**完全分不出差别**，
  //    等于白测。改成「毫秒/轮」后量纲直接可读，也不存在截断问题。
  static unsigned long lastHeartbeat = 0;
  static uint32_t      lastLoopCount = 0;
  if (millis() - lastHeartbeat >= 60000UL) {
    unsigned long elapsed = millis() - lastHeartbeat;   // 首次即 60s，与 lastLoopCount=0 对齐
    uint32_t delta  = loopCount - lastLoopCount;
    uint32_t loopMs = delta ? (uint32_t)(elapsed / delta) : 0;   // 0 = 一轮都没跑，异常
    lastHeartbeat = millis();
    lastLoopCount = loopCount;
    Serial.printf("[HB] uptime=%lus loopMs=%u free=%u maxAlloc=%u asyncStackFree=%uB rssi=%d\n",
                  millis() / 1000UL, loopMs,
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap(),
                  (g_asyncStackHWM == 0xFFFFFFFF) ? 0U : (unsigned)g_asyncStackHWM,
                  WiFi.RSSI());
    // rssi 判读：>-60 好 / -60~-70 一般 / -70~-80 差 / <-80 很差，丢包重传会明显
    // 若 rssi 很差，应用层做什么优化都没用 —— 先解决射频（挪位置/换 AP/换天线）

    // ---- Pi 请求耗时（与上一条 [HB] 同一个 60s 窗口，打完清零）----
    // ⚠️ 故意另起一行而不是并进 [HB]：analyze_longrun.py 按固定格式解析 [HB]，
    //    改那一行的格式会把分析脚本整个弄坏。
    if (piN || dcN) {
      Serial.printf("[PI] %lus窗口 行情 n=%lu ok=%lu bad=%lu 慢(%lums+)=%lu "
                    "avg=%lums max=%lums | 直连 n=%lu avg=%lums max=%lums\n",
                    elapsed / 1000UL,
                    (unsigned long)piN, (unsigned long)piOk, (unsigned long)piFail,
                    PI_SLOW_MS, (unsigned long)piSlow,
                    (unsigned long)(piN ? piSum / piN : 0ULL), (unsigned long)piMax,
                    (unsigned long)dcN,
                    (unsigned long)(dcN ? dcSum / dcN : 0ULL), (unsigned long)dcMax);
      // 分段均值 —— 定位那 ~1.79 秒花在哪一段（piN 为分母；失败路径没有 body 段，记 0）
      if (piN) {
        Serial.printf("[PISEG] n=%lu conn=%lums http=%lums body=%lums end=%lums"
                      " | 峰值 conn=%lums http=%lums body=%lums\n",
                      (unsigned long)piN,
                      (unsigned long)(piSumConn / piN), (unsigned long)(piSumHttp / piN),
                      (unsigned long)(piSumBody / piN), (unsigned long)(piSumEnd / piN),
                      (unsigned long)piMaxConn, (unsigned long)piMaxHttp,
                      (unsigned long)piMaxBody);
      }
    }
    piN = piOk = piFail = piSlow = piMax = 0; piSum = 0;
    piSumConn = piSumHttp = piSumBody = piSumEnd = 0;
    piMaxConn = piMaxHttp = piMaxBody = 0;
    dcN = dcMax = 0; dcSum = 0;

    // ---- [RTT] 裸 TCP 往返探针：连续两次连接，各自计时 ----
    // 为什么要连两次：区分「空闲后首包慢」和「每个连接都慢」——
    //   第1次慢、第2次快 ⇒ 空闲后的首包问题（ARP 过期 / AP 缓冲 / 省电）
    //   两次都慢         ⇒ 链路 RTT 本身就是这么大（射频/AP 侧）
    // 实测背景：PC 侧同 AP 同频段 TCP 握手只要 10~20ms，本机 conn 段 526ms，差 35 倍。
    // 连的是已知打开的端口，连上立刻 stop()，Flask 只会看到一个空连接，无害。
    {
      IPAddress rIp;
      if (rIp.fromString(piHubHost)) {
        unsigned long r1 = 0, r2 = 0; bool o1 = false, o2 = false;
        { WiFiClient w; unsigned long t = millis();
          o1 = w.connect(rIp, (uint16_t)piHubPort, 1000); r1 = millis() - t; w.stop(); }
        { WiFiClient w; unsigned long t = millis();
          o2 = w.connect(rIp, (uint16_t)piHubPort, 1000); r2 = millis() - t; w.stop(); }
        wifi_ps_type_t psNow = (wifi_ps_type_t)-1;
        esp_wifi_get_ps(&psNow);
        Serial.printf("[RTT] 裸TCP往返 第1次=%lums(ok=%d) 第2次=%lums(ok=%d) "
                      "当前省电PS=%d rssi=%d\n",
                      r1, (int)o1, r2, (int)o2, (int)psNow, WiFi.RSSI());
      }
    }
  }

  // 延迟重启(上传/保存股票/重置WiFi 后)
  if (pendingRestartAt != 0 && millis() >= pendingRestartAt) {
    // ⚠️ 埋点：这里曾经在压测中出现过不明原因的反复重启（rst:0x3 软件复位），
    //    而三个设置 pendingRestartAt 的入口（保存股票/重置WiFi/上传完成）都没被调用。
    //    打印出来才能定位是"入口被命中"还是"变量被写坏"。见 PERFORMANCE.md INC-008。
    Serial.printf("[RST] loop 触发重启: pendingRestartAt=%lu now=%lu wifiReset=%d\n",
                  pendingRestartAt, millis(), (int)pendingWiFiReset);
    serialDrain(40);                // ⚠️ 必须排在 ESP.restart() 之前：
                                    //    restart 会立刻复位，未排空的 TX 缓冲被丢弃 ——
                                    //    上一轮 [RST] 那行就是这样丢的（串口里确实查不到）。
                                    //    但【不能】用 Serial.flush()：CDC 下它无超时会死锁。
    if (pendingWiFiReset) {
      prefs.clear();                // 清除所有 NVS 配置（包括 WiFi）
      WiFi.disconnect(true, true);  // 断开并清除 WiFi 凭据
      delay(100);
    }
    ESP.restart();
  }

  // WiFi 切换测试(最长 30 秒阻塞)
  if (pendingWiFiSwitch) {
    pendingWiFiSwitch = false;
    doWiFiSwitch(pendingNewSSID, pendingNewPass);
  }

  // 模式切换 / 壁纸重绘
  if (pendingModeChange) {
    pendingModeChange = false;
    g_rtcStage = 10;                 // 死在 10 = setMode() 内部
    setMode(pendingMode);
    g_rtcStage = 11;
  }
  // 天气立即刷新(切换城市后)
  if (pendingWeatherRefresh) {
    pendingWeatherRefresh = false;
    if (currentMode == MODE_WEATHER) renderWeather();
  }
  // 股票视图切换后重绘
  if (pendingStockRefresh) {
    pendingStockRefresh = false;
    if (currentMode == MODE_STOCK) drawQuote(stockCurIdx, true);
  }
  // 开启自动亮度后立即调整一次
  if (pendingBrightnessAdjust) {
    pendingBrightnessAdjust = false;
    autoAdjustBrightness();
  }

  // 自动亮度调节(每分钟检查一次)
  static unsigned long lastBrightnessCheck = 0;
  if (millis() - lastBrightnessCheck >= 60000UL) {
    autoAdjustBrightness();
    lastBrightnessCheck = millis();
  }

  // 动态壁纸轮播(时钟/天气模式 + 动态壁纸开启)
  if (wallpaperMode == 2 && (currentMode == MODE_CLOCK || currentMode == MODE_WEATHER)) {
    static unsigned long lastWallRotate = 0;
    if (millis() - lastWallRotate >= 5000UL) {
      wallpaperIndex = (wallpaperIndex + 1) % 3;
      showWallpaper();
      if (currentMode == MODE_CLOCK) {          // 重叠加时钟
        lastH = lastM = lastS = lastD = -1;
        drawColon();
        digitalClockDisplay();
      } else if (w.ok) {                         // 重叠加天气
        drawWeather();
      }
      lastWallRotate = millis();
    }
  }

  g_rtcStage = 20; g_rtcStageFree = ESP.getFreeHeap(); g_rtcStageMax = ESP.getMaxAllocHeap();
  renderCurrentMode();
  g_rtcStage = 30;
  delay(50);
}
