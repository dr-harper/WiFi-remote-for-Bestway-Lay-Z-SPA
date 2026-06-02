#pragma once

// In-memory ring buffer of recent log lines.
//
// Built to substitute for USB-serial debug on Lay-Z-Spa adapters that
// hardware-isolate the CH340's RX during normal operation (after-boot
// only the bootloader path is wired; ESP TX never reaches the USB
// host). Without this we have zero visibility into what the firmware
// is doing at runtime.
//
// Buffer size is bounded — 100 lines × ~120 chars ≈ 12 KB peak. Lives
// in RAM (no LittleFS writes per line — that'd wear the flash and the
// log volume is high during WiFi events).
//
// Usage:
//   logbuf::setup();
//   logbuf::write("got here");
//   logbuf::writef("WiFi status=%d rssi=%d", st, rssi);
//   String s = logbuf::dump();   // newest-last, ready to display

#include <Arduino.h>

namespace logbuf {

void setup();
void write(const String &line);
void write(const char *line);
void writef(const char *fmt, ...);   // printf-style; line length capped at 200 chars
String dump();                       // every retained line, oldest-first, '\n'-separated
size_t lineCount();                  // for diagnostics

}   // namespace logbuf
