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
bool flashStatusCheck(bool clearIfError = true, bool echo = true, bool useExistingStatus = false) {
  bool ok = true;
  if (!useExistingStatus) flashCommand(lastAddr, 0x70);
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
    // delayMicroseconds(us);
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

  if (!flashStatusCheck(true, true, true)) {
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

// Should only be used to start a multibyte write, diff command than general status checking
// Technically we could utilize both write buffers and continue to write even if busy reported
// Exponential backoff
bool flashSetupMultiByteWrite(uint32_t addr, uint32_t timeout_sec = 60) {
  uint32_t statusStart = millis();
  uint32_t delayUs = 2;
  while (true) {
    // First, multi word/byte write setup (E8H) is written with
    // the write address. At this point, the device
    // automatically outputs extended status register data
    // (XSR) when read
    flashCommand(addr, 0xe8);
    flashReadStatus();

    // If extended
    // status register bit XSR.7 is 0, no Multi Word/Byte
    // Write command is available and multi word/byte write
    // setup which just has been written is ignored. To retry,
    // continue monitoring XSR.7 by writing multi word/byte
    // write setup with write address until XSR.7 transitions
    // to 1. When XSR.7 transitions to 1, the device is ready
    // for loading the data to the buffer.
    if (SR(7)) {
      return true;
    }

    if (millis() - statusStart > timeout_sec * 1000) {
      // Only fall through here if timeout
      if (SR(1) && SR(4)) {
        sprintf(S, "Block lock error @ %06x\r\n", lastAddr);
        echo_all();
      }
      if (SR(3) && SR(4)) {
        sprintf(S, "Undervoltage error @ %06x\r\n", lastAddr);
        echo_all();
      }
      if (SR(4) || SR(5)) {
        sprintf(S, "Unable to multibyte write @ %06x\r\n", lastAddr);
        echo_all();
      }

      sprintf(S, "\r\nTIMEOUT STATUS=%x\r\n", SRD);
      echo_all();
      // Done retrying
      return false;
    } else {
      // Retry, exponential backoff
      flashCommand(lastAddr, CMD_STATUS_CLR);
      delayMicroseconds(delayUs *= 2);
    }
  }
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
  bool atBoundary = false;
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
      atBoundary = true;
      bytesToWrite = MIN(bankBoundary - addr, MAX_MULTIBYTE_WRITE);
    }
    int wordsToWrite = bytesToWrite / 2;

    if (!flashSetupMultiByteWrite(addr)) {
      flashCommand(lastAddr, CMD_STATUS_CLR);
      delay(250);
      flashClearLocks();
      delay(1000);
      echo_all("\rRetrying...\r");
      continue;
    }

    // XSR.7 == 1 now, ready for write
    // A word/byte count (N)-1 is written with write address.
    flashCommand(addr, wordsToWrite - 1);

    // On the next write, device start address is written with buffer data.
    // Subsequent writes provide additional device address and data, depending on the count.
    // All subsequent address must lie within the start address plus the count.
    setControl(ROMCE);

    const uint8_t ROMCE_ROMWE = ROMCE & ROMWE;
    SPI.beginTransaction(mcpAddr0.mySPISettings);
    for (int j = 0; j < wordsToWrite; j++, addr += 2, bufPtr += 2) {

      uint32_t diff = addr ^ lastAddr;
      // More expensive to do the test than to just send both bytes, and we know the lower bits of the address are changing
      // bool diffA = (diff & 0x000000ff) != 0;
      // if ((diff & 0x0000ff00) != 0) {
      // A0-A15
      mcpAddr0.writeMCP23017_noTransaction(addr & 0xff, (addr >> 8) & 0xff);
      // } else {
      //   // A0-A7 always change
      //   mcpAddr0.writeMCP23017_noTransaction_singlePort(0x12, addr & 0xff);
      // }
      if ((diff & 0x00ff0000) != 0) {
        // A16-A21
        mcpAddr1.writeMCP23017_noTransaction_singlePort(0x12, addr >> 16);
      }
      lastAddr = addr;

      mcpData.writeMCP23017_noTransaction(buf[bufPtr + 1], buf[bufPtr]);
      mcpAddr1.writeMCP23017_noTransaction_singlePort(0x13, ROMCE_ROMWE);
      asm volatile("nop\nnop\nnop\nnop\nnop\nnop\nnop");
      mcpAddr1.writeMCP23017_noTransaction_singlePort(0x13, ROMCE);
    }
    SPI.endTransaction();
    // setControl(IDLE); // already part of flashCommand

    // After the final buffer data is written, write confirm (DOH) must be written.
    // This initiates WSM to begin copying the buffer data to the Flash Array.
    // Use the bank we started with not the bank we ended with
    flashCommand(lastAddr, 0xd0);

    if (atBoundary) {
      flashWaitUntilDone();
      flashCommand(bankBoundary, CMD_STATUS_CLR);
    }
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