// ST7789 240x240 display for ESP32-C3
// ============================================================================
//  ⚠️ 请把本文件覆盖到 Arduino 库目录下的 TFT_eSPI/User_Setup.h
//     （覆盖前建议先备份原 User_Setup.h）
// ============================================================================

// ---------------------------------------------------------------------------
//  ESP32-C3：修正 ESP-IDF 的 REG_SPI_BASE 宏（较新 Arduino core 必加，否则启动即崩）
// ---------------------------------------------------------------------------
//  【症状】用较新的 Arduino-ESP32 core（实测 2.0.17）编译，烧进真机后启动即崩、反复重启：
//      Guru Meditation Error: Core 0 panic'ed (Store access fault)
//      位置在 tft.init() 里 —— TFT_eSPI::begin_tft_write()，
//      即 SET_BUS_WRITE_MODE 解引用 *_spi_user 时越界。
//
//  【根因】TFT_eSPI 2.5.43 的 C3 处理器头里有兜底：
//      #ifndef REG_SPI_BASE
//        #define REG_SPI_BASE(i) DR_REG_SPI2_BASE
//      #endif
//      但较新的 ESP-IDF（core 2.0.17 = IDF v4.4.7）已在 soc.h 里【无守卫】地定义：
//      #define REG_SPI_BASE(i) (((i)==2) ? (DR_REG_SPI2_BASE) : (0))
//      ⇒ 那句 #ifndef 被挡掉，走 SDK 版。
//      而 TFT_eSPI 传进来的下标是 SPI_PORT = SPI2_HOST，
//      本版 IDF 的 hal/spi_types.h 里 `SPI2_HOST = 1`（不是 2）
//      ⇒ 条件 ((1)==2) 恒假 ⇒ 整个宏塌成 0
//      ⇒ _spi_user = 0 + 0x10 = 0x10 —— 这只是"寄存器偏移"，不是地址；
//        解引用它必然 Store access fault。
//      （TFT_eSPI 2.5.43 发布于 2023-12，这个 IDF 改动晚于它，属版本错配，非库的疏忽。）
//
//  【本解法】利用 soc.h 自己带 #pragma once：
//      先把它整个吃掉，之后 TFT_eSPI 再 include 时会被 pragma once 挡掉、
//      不会再展开它的 REG_SPI_BASE；此时我们再把它改成 TFT_eSPI 原本想要的样子。
//      ⇒ 不用改 TFT_eSPI 库，也不用指定 core 版本。
//      （全框架内 REG_SPI_BASE 只有 soc.h 定义、spi_reg.h 使用，影响面仅 SPI 寄存器访问。）
//
//  【为什么必须写在这里】对 Arduino IDE 用户，User_Setup.h 是唯一能注入进
//      TFT_eSPI.cpp 那个编译单元的位置 —— 改主程序 .ino 没用，它是独立的 TU。
//  【兼容性】core 2.0.4（soc.h 里没有那个宏）实测构建结果一字未变 ⇒ 不需要时零影响。
#if defined(ARDUINO_ARCH_ESP32)
  #include <soc/soc.h>   // 顺带带进 sdkconfig.h，供下面的目标判断使用
  #if defined(CONFIG_IDF_TARGET_ESP32C3)
    #ifdef REG_SPI_BASE
      #undef REG_SPI_BASE
    #endif
    #define REG_SPI_BASE(i) DR_REG_SPI2_BASE
  #endif
#endif

#define ST7789_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 240

// Pin definitions for ESP32-C3 (以通电实测为准，详见 README「第零步」)
#define TFT_MOSI 5   // SDA → IO5
#define TFT_SCLK 3   // SCL → IO3
#define TFT_CS   -1  // Connected to GND
#define TFT_DC   2   // DC/RS → IO2
#define TFT_RST  6   // Reset → IO6
#define TFT_BL   1   // Backlight → IO1 (P-MOS controlled)

// Display settings
#define TFT_INVERSION_ON
#define TFT_BACKLIGHT_ON LOW  // P-MOS: LOW = ON

// SPI frequency
#define SPI_FREQUENCY  10000000
#define SPI_READ_FREQUENCY  5000000

// Font
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define SMOOTH_FONT
