// lcd_gfx.cpp — see lcd_gfx.h. Borrowed from smalltv-mod 2.16.0.
//
// The SmallTV's ST7789 has its CS line tied to GND and only latches SPI in
// **mode 3**. Arduino_GFX's stock Arduino_ST7789 forces SPI_MODE2 on the ESP8266
// (wrong clock edge for this panel), so the controller never initializes and the
// screen stays black even with the backlight on. Subclass begin() to force mode 3
// — matching the known-good GeekMagic community firmwares.
#include "lcd_gfx.h"
#include <Arduino_GFX_Library.h>
#include <SPI.h>

// ---- board_esp8266.h: pin map for the GeekMagic SmallTV (ESP-12F) ---------
// ESP8266 HW-SPI fixed pins: SCLK=GPIO14, MOSI=GPIO13.
#define TFT_SCLK 14   // D5  (HW SPI clock, fixed)
#define TFT_MOSI 13   // D7  (HW SPI data,  fixed)
#define TFT_DC   0    // D3  (data/command) — also a boot-strap pin
#define TFT_RST  2    // D4  (reset)        — also a boot-strap pin / onboard LED
#define TFT_CS   15   // D8  (chip select)  — also a boot-strap pin
#define TFT_BL   5    // D1  (backlight, PWM capable)
#define TFT_BGR  0    // panel colour order: RGB

// ---- config.h: panel geometry --------------------------------------------
// The ST7789(V) is a 240x320 controller but the 1.54" glass only wires RAM
// rows 0-239; offset2 (80) keeps rotation 2/3 off the dead band.
#define TFT_COL_OFFSET1 0
#define TFT_ROW_OFFSET1 0
#define TFT_COL_OFFSET2 0
#define TFT_ROW_OFFSET2 80

static bool g_bgr = (TFT_BGR != 0);

class Arduino_ST7789_LCD : public Arduino_ST7789 {
 public:
  using Arduino_ST7789::Arduino_ST7789;   // inherit constructors
  bool begin(int32_t speed = GFX_NOT_DEFINED) override {
    _override_datamode = SPI_MODE3;
    return Arduino_TFT::begin(speed);
  }

  // Re-issue MADCTL with the BGR bit (0x08) tracking g_bgr on every rotation
  // change; the MX/MY/MV values are the base class's own mapping.
  void setRotation(uint8_t r) override {
    Arduino_TFT::setRotation(r);           // updates _rotation + width/height
    uint8_t madctl;
    switch (_rotation) {
      case 1:  madctl = ST7789_MADCTL_MX | ST7789_MADCTL_MV; break;
      case 2:  madctl = ST7789_MADCTL_MX | ST7789_MADCTL_MY; break;
      case 3:  madctl = ST7789_MADCTL_MY | ST7789_MADCTL_MV; break;
      default: madctl = 0; break;          // case 0
    }
    if (g_bgr) madctl |= 0x08;              // BGR
    _bus->beginWrite();
    _bus->writeC8D8(ST7789_MADCTL, madctl);
    _bus->endWrite();
  }
};

static Arduino_DataBus* bus = nullptr;
static Arduino_GFX*     gfx = nullptr;

Arduino_GFX* lcdDev() { return gfx; }

void lcdBegin() {
  // Backlight FIRST: a dark backlight then means the sketch never got this far.
  // Active-low (TFT_BL_DEFAULT_INVERTED): duty 0 = full on, 255 = off.
  pinMode(TFT_BL, OUTPUT);
  analogWriteRange(255);
  analogWrite(TFT_BL, 255 - (90 * 255 / 100));   // brightness 90%

  bus = new Arduino_HWSPI(TFT_DC, TFT_CS);   // ESP8266 HW-SPI (fixed SCLK/MOSI)
  // IPS=true so the panel colors are not inverted.
  gfx = new Arduino_ST7789_LCD(bus, TFT_RST, 0 /*rotation*/, true /*IPS*/,
                               LCD_W, LCD_H,
                               TFT_COL_OFFSET1, TFT_ROW_OFFSET1,
                               TFT_COL_OFFSET2, TFT_ROW_OFFSET2);
  gfx->begin();
  gfx->setRotation(0);
  gfx->invertDisplay(false);
  gfx->setTextWrap(false);   // overflowing labels must not wrap to the left edge
  gfx->fillScreen(0x0000);  // black, before lcd_ui.h's drawSplash() paints over it
}
