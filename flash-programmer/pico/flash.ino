// Make sure the BE1 don't keep getting twiddled back and forth,
// use 0 as a command address BUT KEEP BE1 SET
inline uint32_t zeroWithBank(uint32_t addr) {
  return addr & FLASH_BANK_SIZE;
}

// Bus states - Sharp flash uses 3 wire control and these are predefined states

inline void busRead() {
  setControl(ROMCE & OE);
  databusReadMode();
}

inline void busWrite() {
  setControl(ROMCE & ROMWE);
  databusWriteMode();
}

inline void busIdle() {
  setControl(IDLE);
  databusWriteMode();
}

// The two basic building blocks: read a word, and send a command / write a word

uint16_t flashReadWord(uint32_t addr) {
  busIdle();

  setAddress(addr);

  busRead();

  // tAVQV | Address to output delay | MAX 100ns
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;

  uint16_t data = readWord();
  busIdle();

  // tAVAV | Read Cycle Time | MIN 120ns
  NOP;
  NOP;
  NOP;

  return data;
}

void flashCommand(uint32_t addr, uint16_t data) {
  busIdle();

  // tEHEL | BE# Pulse Width High | Min 25ns
  // This will easily be accomplished during the following writeWord.
  NOP;
  NOP;
  NOP;
  NOP;

  writeWord(addr, data);

  busWrite();

  // tELEH | BE# Pulse Width | Min 70ns
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;

  busIdle();
}

void flashReadStatus() {
  busRead();
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;
  NOP;
  SRD = readByte();
  busIdle();
}

// returns TRUE if OK
bool flashStatusCheck(bool clearIfError = true, bool echo = true) {
  bool ok = true;
  flashCommand(lastAddr, 0x70);
  flashReadStatus();
  if (!SR(7)) {
    if (echo) echo_all("STATUS busy\r\n");
    ok = false;
  }
  if (SR(5) && SR(4)) {
    if (echo) echo_all("STATUS improper command\r\n");
    ok = false;
  }
  if (SR(3)) {
    if (echo) echo_all("STATUS undervoltage\r\n");
    ok = false;
  }
  if (SR(1)) {
    if (echo) echo_all("STATUS locked\r\n");
    ok = false;
  }
  if (SR(2)) {
    if (echo) echo_all("STATUS write suspended\r\n");
    ok = false;
  }
  if (SR(6)) {
    if (echo) echo_all("STATUS erase suspended\r\n");
    ok = false;
  }
  if (!ok && clearIfError) {
    // clear status register
    flashCommand(lastAddr, CMD_STATUS_CLR);
  }
  return ok;
}

void flashWaitUntilDone() {
  do {
    delayMicroseconds(1);
    flashReadStatus();
  } while (!SR(7));
}

// Major functions

void flashEraseBank(int bank) {
  int32_t bankAddress = bank ? FLASH_BANK_SIZE : 0;
  sprintf(S, "Erase bank %d", bank);
  echo_all();

  delayMicroseconds(100);
  flashCommand(bankAddress, 0x70);
  flashWaitUntilDone();
  delayMicroseconds(100);

  delayMicroseconds(100);
  flashCommand(bankAddress, 0x30);
  delayMicroseconds(100);
  flashCommand(bankAddress, 0xd0);

  const int ERASE_TIMEOUT_MS = 30000;
  const int CHECK_INTERVAL_MS = 1000;
  for (int time = 0; time < ERASE_TIMEOUT_MS; time += CHECK_INTERVAL_MS) {
    flashReadStatus();
    if (SR(7)) {
      echo_all("Erased\r");
      break;
    } else {
      echo_all(".");
      delay(CHECK_INTERVAL_MS);
    }
  }

  if (!flashStatusCheck()) {
    echo_all("Bank erase error\r");
  }

  // if (SR(4) == 1 && SR(5) == 1) {
  //   echo_all("Invalid Bank Erase command sequence\r");
  // } else if (SR(5) == 1) {
  //   echo_all("Bank Erase error\r");
  // } else {
  //   echo_all("Bank Erase successful!\r");
  // }

  // clear status register
  // flashCommand(0, CMD_STATUS_CLR);
  // delayMicroseconds(100);
  // Read mode
  flashCommand(0, CMD_RESET);
  delayMicroseconds(100);
}

void flashEraseAll() {
  stopwatch = millis();
  flashEraseBank(0);
  flashEraseBank(1);
  sprintf(S, "Erased in %0.2fs\r\n", (millis() - stopwatch) / 1000.0);
  echo_all();
}

bool flashEraseBlock(uint32_t startAddr) {
  //startAddr = startAddr & ~(FLASH_BLOCK_SIZE - 1);
  // sprintf(S, "Erase block %06xh-%06xh\r\n", startAddr, startAddr + FLASH_BLOCK_SIZE - 1);
  // echo_all();

  flashCommand(startAddr, 0x20);
  flashCommand(startAddr, 0xd0);
  flashWaitUntilDone();

  return flashStatusCheck();
}

void flashClearLocks() {
  echo_all("Clearing lock bits...");
  flashCommand(0, 0x60);
  flashCommand(0, 0xd0);
  flashWaitUntilDone();

  if (flashStatusCheck()) {
    echo_all("Cleared\r");
  } else {
    echo_all("FAIL\r");
  }
}

// This should display the manufacturer and device code of the FLASH CHIP: B0 D0
void flashChipId() {
  busIdle();
  delayMicroseconds(100);

  flashCommand(0, 0x90);
  delayMicroseconds(100);

  busRead();
  delayMicroseconds(100);

  uint16_t manufacturer = flashReadWord(0);

  busIdle();
  delayMicroseconds(100);

  delay(100);
  uint16_t device = flashReadWord(2);

  databusWriteMode();
  flashCommand(0, CMD_RESET);
  delay(100);

  sprintf(S, "Manufacturer=%x Device=%x\r", manufacturer, device);
  echo_all();
}

// Does the cart start with 0E00 0080 like a loopy cart?
bool flashCartHeaderCheck() {
  return (flashReadWord(0x0) == 0x0e00 && flashReadWord(0x2) == 0x0080);
}

// What's the internal CRC32 in the cart header? Use this as a cart ID.
uint32_t flashCartHeaderId() {
  return (flashReadWord(0x8) << 16) | flashReadWord(0xA);
}

uint32_t flashCartHeaderSramSize() {
  uint32_t sramStart = (flashReadWord(0x10) << 16 | flashReadWord(0x12));
  uint32_t sramEnd = (flashReadWord(0x14) << 16 | flashReadWord(0x16));
  return sramEnd - sramStart + 1;
}

void flashInspect(uint32_t starting, uint32_t upto) {
  flashCommand(zeroWithBank(starting), 0xff);
  delayMicroseconds(1);

  for (uint32_t addr = starting; addr < upto; addr += 2) {
    if (addr % 0x10 == 0) {
      sprintf(S, "\r%06xh\t\t", addr);
      echo_all();
    }
    sprintf(S, "%04x\t", flashReadWord(addr));
    echo_all();
  }
}

void flashDump(uint32_t starting = 0, uint32_t upto = FLASH_SIZE) {
  uint32_t bank = ~0;

  for (uint32_t addr = starting; addr < upto; addr += 2) {
    uint32_t newBank = zeroWithBank(addr);
    if (bank != newBank) {
      bank = newBank;
      flashCommand(bank, CMD_RESET);
      delay(10);
    }
    uint16_t word = flashReadWord(addr);
    usb_web.write(word >> 8);
    usb_web.write(word & 0xff);
  }
  usb_web.flush();
}

inline void writeMCP23017_noTransaction(uint8_t csPin, uint16_t transBytes, uint8_t valA, uint8_t valB) {
  digitalWriteFast(csPin, LOW);
  SPI.transfer16(transBytes);
  SPI.transfer(valA);
  SPI.transfer(valB);
  digitalWriteFast(csPin, HIGH);
}

inline void writeMCP23017_noTransaction_singlePort(uint8_t csPin, uint16_t transBytes, uint8_t val) {
  digitalWriteFast(csPin, LOW);
  SPI.transfer16(transBytes);
  SPI.transfer(val);
  digitalWriteFast(csPin, HIGH);
}

// Returns whether programming should continue
bool flashWriteBuffer(uint8_t *buf, size_t bufLen, uint32_t &addr, uint32_t expectedBytes) {
  if ((bufLen % 2) == 1) {
    sprintf(S, "WARNING: odd number of bytes %d\r", bufLen);
    echo_all();
  }

  if (bufLen <= 0) {
    echo_all("WARNING: Empty buffer\r");
  }

  // echo progress at fixed intervals
  if (addr % 0x08000 == 0) {
    sprintf(S, "%06xh ", addr);
    echo_all();
  }

  // All of these are in BYTES
  // addr - overall address
  // buf - multi-byte buffer sent over by USB
  // bufLen - length of it
  // bufPtr - step through buffer using this

  // Multi-word write can write up to 32 bytes / 16 words
  const uint32_t bankBoundary = FLASH_BANK_SIZE;
  const size_t MAX_MULTIBYTE_WRITE = 32;
  uint32_t currentBank = zeroWithBank(addr);

  // Do however many multibyte writes necessary to empty the buffer
  for (int bufPtr = 0; bufPtr < bufLen;) {

    // Skip blocks of 0xff *between* multibyte writes only, assuming flash has been erased
    if (bufPtr + 1 < bufLen && buf[bufPtr] == 0xff && buf[bufPtr + 1] == 0xff) {
      bufPtr += 2;
      addr += 2;
      continue;
    }

    int bytesToWrite = MIN(bufLen - bufPtr, MAX_MULTIBYTE_WRITE);
    // Don't allow multibyte writes to cross banks
    if (addr < bankBoundary && addr + bytesToWrite >= bankBoundary) {
      bytesToWrite = MIN(bankBoundary - addr, MAX_MULTIBYTE_WRITE);
    }
    int wordsToWrite = bytesToWrite / 2;

    // To retry,
    // continue monitoring XSR.7 by writing multi word/byte
    // write setup with write address until XSR.7 transitions
    // to 1.
    do {
      flashCommand(addr, 0xe8);
      flashReadStatus();
    } while (!SR(7));

    // XSR.7 == 1 now, ready for write
    // A word/byte count (N)-1 is written with write address.
    flashCommand(addr, wordsToWrite - 1);

    // On the next write, device start address is written with buffer data.
    // Subsequent writes provide additional device address and data, depending on the count.
    // All subsequent address must lie within the start address plus the count.
    setControl(ROMCE);

    // NOTE -- the following tight loop, writing the 32 byte buffer, is optimized at a low level
    // It performs the same function as:
    // for (int j = 0; j < wordsToWrite; j++, addr += 2, bufPtr += 2) {
    //   flashCommand(addr, buf[bufPtr] << 8 | buf[bufPtr + 1]);
    // }

    const uint8_t ROMCE_ROMWE = ROMCE & ROMWE;
    const uint8_t MCP_GPIOA = 0x12;
    const uint8_t MCP_GPIOB = 0x13;

    SPI.beginTransaction(mcpSettings);
    for (int j = 0; j < wordsToWrite; j++, addr += 2, bufPtr += 2) {

      // A0-A15
      const uint16_t MCP_REG_ADDR0_AB = ((MCP_ADDR_ADDR0 << 9) | MCP_GPIOA);
      writeMCP23017_noTransaction(MCP_CS_ADDR0, MCP_REG_ADDR0_AB,
                                  addr & 0xff, (addr >> 8) & 0xff);

      // If the last 15 digits are 0, the 16th digit rolled over
      // This replaces a slower test on the XOR of the previous address - we know we're going up by 2s
      if ((addr & 0xfffe) == 0) {
        // A16-A21
        const uint16_t MCP_REG_ADDR1_A = ((MCP_ADDR_ADDR1 << 9) | MCP_GPIOA);
        writeMCP23017_noTransaction_singlePort(MCP_CS_ADDR1, MCP_REG_ADDR1_A,
                                               addr >> 16);
      }
      lastAddr = addr;

      // DATA (2 bytes)
      const uint16_t MCP_REG_DATA_AB = ((MCP_ADDR_DATA << 9) | MCP_GPIOA);
      writeMCP23017_noTransaction(MCP_CS_DATA, MCP_REG_DATA_AB,
                                  buf[bufPtr + 1], buf[bufPtr]);

      // WE LOW
      const uint16_t MCP_REG_ADDR1_B = ((MCP_ADDR_ADDR1 << 9) | MCP_GPIOB);
      writeMCP23017_noTransaction_singlePort(MCP_CS_ADDR1, MCP_REG_ADDR1_B,
                                             ROMCE_ROMWE);

      // 40ns / 4.0e-8s minimum holding down WE is required
      // 13 cycles @300Mhz (3.3e-9s), <1 cycle @12Mhz (8.3e-8s) (SPI clock)
      // If you OC to 24Mhz (4.1e-8s) this is dangerously close to one clock cycle
      NOP;
      NOP;
      NOP;
      NOP;
      NOP;
      NOP;
      NOP;
      NOP;
      NOP;
      NOP;
      NOP;
      NOP;

      // WE HIGH
      writeMCP23017_noTransaction_singlePort(MCP_CS_ADDR1, MCP_REG_ADDR1_B,
                                             ROMCE);
    }
    SPI.endTransaction();
    // setControl(IDLE); // already part of flashCommand

    // After the final buffer data is written, write confirm (DOH) must be written.
    // This initiates WSM to begin copying the buffer data to the Flash Array.
    // Use the bank we started with not the bank we ended with
    flashCommand(lastAddr, 0xd0);
  }

  // Buffer empty, return to main loop, or finish up
  if (addr >= expectedBytes) {
    echo_all("\rFinishing...\r");
    flashWaitUntilDone();
    flashCommand(lastAddr, CMD_RESET);

    double sec = (millis() - stopwatch) / 1000.0;
    sprintf(S, "\r\nWrote %d bytes in %0.2f sec (%0.1f KB/s)\r\n", addr, sec, addr / sec / 1024.0);
    echo_all();

    flashCommand(0, CMD_RESET);
    return false;
  }

  return true;
}