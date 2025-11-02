// ARDUINO BUILD SETTINGS
//   Earle Philhower Arduino-Pico core (board manager: https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json)
//   Waveshare RP2040 Zero (actual hardware is RP2040 Tiny)
//   Flash Size: 2MB (Sketch: 1MB, FS: 1MB)
//   CPU Speed: 240 MHz (Overclock)
//   Optimize: Optimize More (-O2)
//   USB Stack: Adafruit TinyUSB

#include <SPI.h>
#include <MCP23S17.h>  // MCP23017_WE library https://docs.arduino.cc/libraries/mcp23017_we/
#include <Adafruit_TinyUSB.h>
#include <Adafruit_NeoPixel.h>
#include <LittleFS.h>
#include "generated.h"

#define HW_REVISION 8
// The MCP23S17 is rated for 10MHz, overclock it
#define SPI_SPEED 24 * 1000 * 1000
#define DEBUG_LED

// Delays one clock cycle or 7ns @ 133MhZ = 0.000000007518797sec = 7.518797ns
// #define NOP __asm__("nop\n\t")
// 4.17ns @ 240Mhz, double to 8.33ns so we don't have to change number of NOPs
#define NOP __asm__("nop\nnop\n")
#define HALT while (true)

#define ADDRBITS 22
// 64kb blocks
#define FLASH_BLOCK_SIZE 0x010000
// two 2mb banks
#define FLASH_BANK_SIZE 0x200000
#define FLASH_SIZE FLASH_BANK_SIZE << 1
#define CMD_RESET 0xff
#define PIN_INSERTED 14
#define USB_BUFSIZE 64

WEBUSB_URL_DEF(landingPage, 1, "f.loopy.land");
Adafruit_USBD_WebUSB usb_web;

// ~RESET is pulled high when pico is powered up, >=99 means dummy reset pin
#define MCP_NO_RESET_PIN 100
// SPI addresses for MCP23017 are: 0 1 0 0 A2 A1 A0
// Note we almost certainly don't need differing addresses any more since REV6 gave each a unique CE, but no harm keeping it
MCP23S17 mcpData = MCP23S17(5, MCP_NO_RESET_PIN, 0b0100000);   //Data IO, D0-D15, Address 0x0
MCP23S17 mcpAddr0 = MCP23S17(6, MCP_NO_RESET_PIN, 0b0100001);  //Address IO, A0-A15, Address 0x1
MCP23S17 mcpAddr1 = MCP23S17(7, MCP_NO_RESET_PIN, 0b0100010);  //Address and control IO, A16-A21, OE, RAMCE, RAMWE, ROMCE, ROMWE, RESET, Address 0x2

// sprintf buffer
char S[USB_BUFSIZE];
// timing operations
unsigned long stopwatch;

// status register
uint16_t SRD;
#define SR(n) bitRead(SRD, n)

#define RED  0xFF0000
#define BLUE 0x0000FF
Adafruit_NeoPixel led(1, 16, NEO_GRB);

inline void ledColor(uint32_t c) {
  led.setPixelColor(0, c);
  led.show();
}

void flashLed(int n, int d = 100) {
  for (int i = 0; i < n; i++) {
    led.setPixelColor(0, 0xff0000);
    led.show();
    delay(d / 2);
    ledColor(0);
    led.show();
    delay(d / 2);
  }
}

// Changing the port mode is superslow so let's cache it
enum DataBusMode { READ,
                   WRITE };
DataBusMode databusMode = WRITE;

inline void ioReadMode(MCP23S17 *mcp) {
  mcp->setPortMode(0, A);
  mcp->setPortMode(0, B);
}

inline void databusReadMode() {
  if (databusMode == READ) return;
  ioReadMode(&mcpData);
  databusMode = READ;
}

inline void ioWriteMode(MCP23S17 *mcp) {
  mcp->setPortMode(0b11111111, A);
  mcp->setPortMode(0b11111111, B);
}

inline void databusWriteMode() {
  if (databusMode == WRITE) return;
  ioWriteMode(&mcpData);
  databusMode = WRITE;
}

// NOTE address is in bytes, though we write words
inline void setAddress(uint32_t addr) {
  // cache address and only set bits that changed
  static uint32_t lastAddr = 0;
  uint32_t diff = addr ^ lastAddr;

  // A0-A7
  if ((diff & 0x000000ff) != 0) {
    mcpAddr0.setPort(addr & 0xff, A);
  }
  // A8-A15
  if ((diff & 0x0000ff00) != 0) {
    mcpAddr0.setPort((addr >> 8) & 0xff, B);
  }

  // A0-A15
  // if ((diff & 0x0000ffff) != 0) {
  //   mcpAddr0.setPort(addr & 0xff, (addr >> 8) & 0xff);
  // }

  // A16-A21
  if ((diff & 0x00ff0000) != 0) {
    mcpAddr1.setPort((addr >> 16) & 0xff, A);
  }

  lastAddr = addr;
}

// Control pins migrated to io expanders so build a bitmask with these. They're all active low so & these:
const uint8_t IDLE = 0xff;
const uint8_t OE = ~(1 << 0);
const uint8_t RAMCE = ~(1 << 1);
const uint8_t RAMWE = ~(1 << 2);
const uint8_t ROMCE = ~(1 << 3);
const uint8_t ROMWE = ~(1 << 4);
const uint8_t RESET = ~(1 << 5);

// Any bits in bitmask that are 0 will go low (active), bits are 0=OE 1=RAMCE 2=RAMWE 3=ROMCE 4=ROMWE 5=RESET
inline void setControl(uint8_t bitmask = IDLE) {
  static uint8_t lastControl = 0;
  if (bitmask != lastControl) {
    mcpAddr1.setPort(bitmask, B);
  }
  lastControl = bitmask;
}

inline uint16_t readWord() {
  return (mcpData.getPort(B) << 8) | mcpData.getPort(A);
}

inline uint8_t readByte() {
  return mcpData.getPort(A);
}

inline void writeWord(uint32_t addr, uint16_t word) {
  setAddress(addr);
  // In testing, caching last write is slower
  mcpData.setPort(word & 0xff, (word >> 8) & 0xff);
}

inline void writeByte(uint32_t addr, uint8_t byte) {
  setAddress(addr);
  mcpData.setPort(byte, A);
}

// Ensures strings are sent modulo the USB buffer size
// Pass no parameters to use the shared string buffer S (after sprintf for example)
void echo_all(const char *buf = NULL) {
  if (!usb_web.connected()) return;

  if (buf == NULL) {
    buf = S;
    // S is already the correct size
  } else {
    // A string is provided, this will pad it to buffer size and fill with NUL
    strncpy(S, buf, USB_BUFSIZE);
  }

  usb_web.write(S, USB_BUFSIZE);
  usb_web.flush();

  // if (Serial) {
  //   Serial.write(buf);
  //   Serial.flush();
  // }
}

void echo_ok() {
  echo_all("!OK\r");
}