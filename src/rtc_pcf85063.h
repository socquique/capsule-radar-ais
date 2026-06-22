#pragma once
// PCF85063 RTC (I2C 0x51, shared bus). Keeps time across power loss so the clock
// and date are correct even before (or without) a WiFi/NTP sync.
// All reads/writes must happen on core 1 (the LVGL loop), never in adsb_task.
#include <time.h>

bool rtc_begin();                       // probe the PCF85063 on the shared I2C bus
bool rtc_present();
bool rtc_read(struct tm *outUtc);       // read UTC; false if absent or clock integrity lost
bool rtc_write(const struct tm *utc);   // set UTC (clears the oscillator-stop flag)
