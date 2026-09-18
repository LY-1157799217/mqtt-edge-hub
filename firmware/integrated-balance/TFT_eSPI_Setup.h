// ST7789 240x240 display for ESP32-C3

#define ST7789_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 240

// Pin definitions for ESP32-C3
#define TFT_MOSI 4   // SDA
#define TFT_SCLK 3   // SCL
#define TFT_CS   -1  // Connected to GND
#define TFT_DC   2   // DC/RS
#define TFT_RST  5   // Reset
#define TFT_BL   1   // Backlight (P-MOS controlled)

// Display settings
#define TFT_INVERSION_ON
#define TFT_BACKLIGHT_ON LOW  // P-MOS: LOW = ON

// SPI frequency
#define SPI_FREQUENCY  40000000
#define SPI_READ_FREQUENCY  20000000

// Font
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define SMOOTH_FONT
