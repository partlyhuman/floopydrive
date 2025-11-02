#define ABORT_AFTER_MS 1000

// Keep Adafruit VID, custom PID for programming
#define VID_PROG 0x239A
#define PID_PROG 0xF100

static int bootErrors = 0;

void onSuccess(bool echoOk = true) {
  if (echoOk) echo_ok();
  busIdle();
  ledColor(0);
}

void loop_programming() {
  static uint8_t buf[USB_BUFSIZE];
  static bool isProgrammingFlash = false;
  static bool isProgrammingSram = false;
  static uint32_t addr = 0;
  static uint32_t totalBytes;
  static uint32_t idleSince = 0;


  if (!usb_web.available()) {
    // DEBUGGING idle detect shows the pico is still looping if other things have halted due to error
    if (idleSince == 0) {
      idleSince = millis();
    } else if (millis() - idleSince > ABORT_AFTER_MS) {
      if (isProgrammingFlash || isProgrammingSram) {
        isProgrammingFlash = false;
        isProgrammingSram = false;
        echo_all("!ERROR Aborted programming after no data\r\n");
      }
    }
    return;
  }
  idleSince = 0;

  size_t bufLen = usb_web.read(buf, USB_BUFSIZE);

  if (isProgrammingFlash) {
    isProgrammingFlash = flashWriteBuffer(buf, bufLen, addr, totalBytes);
    if (!isProgrammingFlash) onSuccess();
    return;
  } else if (isProgrammingSram) {
    isProgrammingSram = sramWriteBuffer(buf, bufLen, addr, totalBytes);
    if (!isProgrammingSram) onSuccess();
    return;
  }

  else if (buf[0] == 'E') {
    // ERASE COMMAND
    if (buf[1] == '\r') {
      ledColor(BLUE);
      flashEraseAll();
      flashCommand(0, CMD_RESET);
      onSuccess();
    } else if (buf[1] == 's' && buf[2] == '\r') {
      ledColor(BLUE);
      sramErase();
      onSuccess();
    } else if (buf[1] == '0' && buf[2] == '\r') {
      // DEPRECATED
      ledColor(BLUE);
      flashEraseBank(0);
      flashCommand(0, CMD_RESET);
      onSuccess();
    } else if (buf[1] == '1' && buf[2] == '\r') {
      // DEPRECATED
      ledColor(BLUE);
      flashEraseBank(1);
      flashCommand(0, CMD_RESET);
      onSuccess();
    } else if (sscanf((char *)buf, "E%ld\r", &totalBytes) && totalBytes > 0 && totalBytes <= FLASH_SIZE) {
      stopwatch = millis();
      sprintf(S, "Erasing %d bytes of flash up to %06xh\r\n", totalBytes, totalBytes);
      echo_all();
      ledColor(BLUE);
      for (addr = 0; addr < totalBytes; addr += FLASH_BLOCK_SIZE) {
        sprintf(S, "%06xh ", addr);
        echo_all();
        if (!flashEraseBlock(addr)) {
          echo_all("\r\n!ERR ABORTING ERASE\r\n");
          return;
        }
      }
      // // NOTE: Erasing by bank isn't faster than erasing by block, so this adds complexity but saves no time
      // addr = 0;
      // while (addr < totalBytes) {
      //   sprintf(S, "%06xh ", addr);
      //   echo_all();
      //   if (addr == 0 && totalBytes >= FLASH_BANK_SIZE) {
      //     flashEraseBank(0);
      //     addr += FLASH_BANK_SIZE;
      //   } else if (addr == FLASH_BANK_SIZE && totalBytes == FLASH_BANK_SIZE * 2) {
      //     flashEraseBank(1);
      //     addr += FLASH_BANK_SIZE;
      //   } else {
      //     if (!flashEraseBlock(addr)) {
      //       echo_all("\r\n!ERR ABORTING ERASE\r\n");
      //       return;
      //     }
      //   }
      // }
      sprintf(S, "\r\nErased %d bytes in %0.2f sec using block erasing\r\n", addr, (millis() - stopwatch) / 1000.0);
      echo_all();
      onSuccess();
    }
  }

  else if (buf[0] == 'I' && buf[1] == '\r') {
    // INSPECT COMMAND
    // flashId();
    echo_all("\r\n---CART-ID---\r\n");
    sprintf(S, "Cart header %s\r\n", flashCartHeaderCheck() ? "OK" : "NOT OK");
    echo_all();
    sprintf(S, "Cart ID %08x\r\n", flashCartHeaderId());
    echo_all();
    echo_all("\r\n---FLASH-lower---\r\n");
    flashInspect(0x000000, 0x000100);
    echo_all("\r\n------\r\n");
    flashInspect(0x100000, 0x100100);
    echo_all("\r\n---FLASH-upper---\r\n");
    flashInspect(0x200000, FLASH_BANK_SIZE + 0x000100);
    echo_all("\r\n------\r\n");
    flashInspect(0x300000, FLASH_BANK_SIZE + 0x100100);
    echo_all("\r\n---SRAM---\r\n");
    sramInspect(0, 0x100);
    echo_all("\r\n---FILESYSTEM---\r\n");
    Dir dir = LittleFS.openDir("/");
    while (dir.next()) {
      echo_all(dir.fileName().c_str());
      echo_all("\r\n");
    }

    // Some info in the info64 struct comes out as zeroes
    // FSInfo64 info;
    // LittleFS.info64(info);
    FSInfo info;
    LittleFS.info(info);
    sprintf(S, "%d/%d bytes used, %0.0f%% full\r\n", info.usedBytes, info.totalBytes, 100.0 * (info.usedBytes + 0.0) / info.totalBytes);
    echo_all();
    onSuccess();
  }

  else if (buf[0] == 'D') {
    // DUMP COMMAND
    if (2 == sscanf((char *)buf, "D%ld/%ld\r", &addr, &totalBytes)) {
      // followed by start address and total # of bytes
      if (addr > 0 && totalBytes > 0 && totalBytes <= FLASH_SIZE) {
        ledColor(BLUE);
        flashDump(addr, addr + totalBytes);
        onSuccess(false);
      } else {
        ledColor(RED);
      }
    } else if (1 == sscanf((char *)buf, "D%ld\r", &totalBytes)) {
      // followed by expected # of bytes
      if (totalBytes > 0 && totalBytes <= FLASH_SIZE) {
        ledColor(BLUE);
        flashDump(0, totalBytes);
        onSuccess(false);
      } else {
        ledColor(RED);
      }
    } else if (buf[1] == 's' && buf[2] == '\r') {
      sramDump();
      onSuccess(false);
    }
  }

  else if (buf[0] == 'P') {
    // PROGRAM COMMAND
    if (1 == sscanf((char *)buf, "P%ld\r", &totalBytes) && totalBytes > 0 && totalBytes <= FLASH_SIZE) {
      // program flash followed by expected # of bytes
      sprintf(S, "Programming %d bytes to flash\r", totalBytes);
      echo_all();
      isProgrammingFlash = true;
      addr = 0;
      stopwatch = millis();
      ledColor(BLUE);
    } else if (1 == sscanf((char *)buf, "Ps%ld\r", &totalBytes) && totalBytes > 0 && totalBytes <= SRAM_SIZE) {
      // program SRAM followed by expected # of bytes
      sprintf(S, "Programming %d bytes to SRAM\r", totalBytes);
      echo_all();
      isProgrammingSram = true;
      addr = 0;
      stopwatch = millis();
      ledColor(BLUE);
    }
  }

  else if (buf[0] == 'S') {
    // SRAM BACKUP & RESTORE FROM FILESYSTEM

    // ID the header
    uint32_t cartId = flashCartHeaderId();

    // we always use <crc32>.sav which is 8 1 3 = 12 chars plus string terminator = 13 bytes
    const size_t SAVE_FILENAME_LEN = 13;
    char filename[SAVE_FILENAME_LEN];
    sprintf(filename, "%08x.sav", cartId);

    if (buf[1] == 'r' && buf[2] == '\r') {
      // backup SRAM to file
      uint32_t sramSize = min(flashCartHeaderSramSize(), SRAM_SIZE);
      sprintf(S, "Backing up first %d bytes of SRAM to %s\r\n", sramSize, filename);
      echo_all();
      ledColor(BLUE);
      sramSaveFile(filename, sramSize);
      onSuccess();
    } else if (buf[1] == 'w' && buf[2] == '\r') {
      // restore SRAM from file
      sprintf(S, "Restoring SRAM contents from %s\r\n", filename);
      echo_all();
      uint32_t sramSize = min(flashCartHeaderSramSize(), SRAM_SIZE);
      ledColor(BLUE);
      sramLoadFile(filename) || sramErase(sramSize);
      onSuccess();
    } else if (buf[1] == 'f' && buf[2] == '\r') {
      // Clear filesystem!
      echo_all("Clearing all save states stored on Floopy Drive!!\r\n");
      ledColor(BLUE);
      LittleFS.format();
      onSuccess();
    }
  }

  else if (buf[0] == 'N') {
    if (sscanf((char *)buf, "N%[^\r]\r", S) && strlen(S) > 0) {
      setNickname(S);
      onSuccess();
    } else {
      setNickname(NULL);
      onSuccess();
    }
  }

  else if (buf[0] != '\r') {
    // Received unknown command OR it's possible we returned to idle state before the sender was done sending?
    echo_all("Unrecognized command\r\n");
    ledColor(0xce4e04);  // orange
  }
}

void onUsbConnect(bool connected) {
  if (connected) {
    char connectStr[USB_BUFSIZE];
    for (int i = 0; i < USB_BUFSIZE; i++) {
      connectStr[i] = '\r';  // make printable
    }
    // Spit out firmware and hardware revisions first thing on connect
    // Ensure we append existing string buffer S which contains any preexisting boot logging
    sprintf(connectStr, "!FW %s\r\n!HW %d\r\n", GIT_COMMIT, HW_REVISION);
    usb_web.write((uint8_t *)connectStr, USB_BUFSIZE);
    usb_web.flush();

    if (bootErrors > 0) {
      echo_all();
      echo_all("\r\n!ERR boot log errors found above\r\n");
    }
  }
}

inline void bootError(const char *str, uint32_t ledErrorColor = 0x800000) {
  bootErrors++;
  sprintf(S, "%s %s", S, str);
  ledColor(ledErrorColor);
}

void setup_programming() {
  // Serial allows us to more easily reset/reprogram the Pico
  // In production, consider reworking echo_all and removing this?
  Serial.begin(115200);

  // Filesystem. Auto formats. Make sure to reserve size in Tools > Flash Size
  // If we wanted to customize the block size could use something like this modifying 4096, perhaps down to 1024
  // FS LittleFS = FS(FSImplPtr(new littlefs_impl::LittleFSImpl(&_FS_start, &_FS_end - &_FS_start, 256, 4096, 16)));
  if (!LittleFS.begin()) bootError("littleFS fail");

  // USB setup
  TinyUSBDevice.setID(VID_PROG, PID_PROG);
  // Reads nickname from FS, must place after LittleFS.begin
  const char *deviceName = makeNicknamedDeviceName();
  TinyUSBDevice.setProductDescriptor(deviceName);
  usb_web.setLandingPage(&landingPage);
  usb_web.setLineStateCallback(onUsbConnect);
  usb_web.setStringDescriptor(deviceName);
  usb_web.begin();

  // Setup IO expanders
  SPI.begin();
  if (!mcpAddr0.Init()) bootError("mcpAddr0 fail");
  if (!mcpAddr1.Init()) bootError("mcpAddr1 fail");
  if (!mcpData.Init()) bootError("mcpData fail");
  mcpAddr0.setSPIClockSpeed(SPI_SPEED);
  mcpAddr1.setSPIClockSpeed(SPI_SPEED);
  mcpData.setSPIClockSpeed(SPI_SPEED);

  ioWriteMode(&mcpData);
  ioWriteMode(&mcpAddr0);

  // CAREFUL what happens when some of these active-low control lines are set momentarily
  busIdle();
  ioWriteMode(&mcpAddr1);
  busIdle();

  // DON'T Backup SRAM contents whenever Pico is booted up - this would ruin things when changing batteries
  // if (!bootErrors && flashCartHeaderCheck()) {
  //   uint32_t cartId = flashCartHeaderId();
  //   if (cartId != 0 && cartId != 0xffffffff) {
  //     ledColor(BLUE);
  //     const size_t SAVE_FILENAME_LEN = 13;
  //     char filename[SAVE_FILENAME_LEN];
  //     sprintf(filename, "%08x.sav", cartId);
  //     sramSaveFile(filename, min(flashCartHeaderSramSize(), SRAM_SIZE));
  //     ledColor(0);
  //   }
  // }

  // Clear flash lock bits preemptively whenever Pico is booted up
  if (!bootErrors) {
    flashClearLocks();
  }

  // Let either claim the USB
  while (!TinyUSBDevice.mounted() && !Serial) {
    delay(1);
  }

  if (!bootErrors) {
    // Give a little flash to indicate boot and connection ok, otherwise leave error status LED unchanged
    ledColor(0xff0000);
    delay(50);
    ledColor(0x00ff00);
    delay(50);
    ledColor(0x0000ff);
    delay(50);
    ledColor(0);
  }
}
