# Hardware

**Identical to Capsule Radar (aircraft).** Use the same board and the **verified** pin map.

- **Board:** Waveshare **ESP32-S3-Touch-AMOLED-1.75** (round 466×466). MCU **ESP32-S3R8** (8 MB PSRAM,
  16 MB flash, dual-core 240 MHz, WiFi + BLE5).
- **Display:** CO5300 AMOLED, 466×466, QSPI. Brightness via panel command (no PWM backlight pin).
- **Touch:** CST9217 (I2C). **IMU:** QMI8658 (I2C). **RTC:** PCF85063 (I2C). **PMIC:** AXP2101
  (I2C 0x34). **Audio:** ES8311 codec + speaker (MX1.25 connector); dual mic on the -C variant.
- **Optional GPS (-G variant):** Quectel **LC76G** GNSS on the shared I2C bus — handy here to
  auto-set the home position. A **working** I2C driver already exists at `PlaneRadar2.0/src/gps.*`
  (the protocol is fiddly — reuse it, don't rewrite). Antenna goes on the **GPS** u.FL, not WiFi.

## Pins / addresses — copy verbatim
Do **not** re-derive these. They are verified and live in
**`/Users/quique/proyectos/PlaneRadar2.0/src/config.h`**:
- LCD: `CS=12, RST=39, SCLK=38, D0=4, D1=5, D2=6, D3=7`; col offset 6.
- Touch: `INT=11, RST=40`, mirror_x/y = true, CST9217 @ **0x5A**.
- Shared I2C: `SDA=15, SCL=14`. IMU 0x6B · RTC 0x51 · PMIC 0x34 · audio ES8311 0x18.
- I2S audio: `MCLK=42, BCLK=9, WS=45, DOUT=8, DIN=10, PA_EN=46`.

## Two board versions (note for users — same gotcha as the aircraft case)
- **Bare PCB ≈ 48.96 mm** diameter — what 3D enclosures are sized for.
- **With Waveshare plastic shell ≈ 51.00 mm** outer — take the board out of the shell before fitting
  a printed enclosure.

## Power / enclosure
- USB-C power + flashing; optional LiPo (3.7 V, MX1.25) charged by the AXP2101.
- Reuse the Capsule Radar enclosure family as a starting point for the marine build.

See `PlaneRadar2.0/docs/HARDWARE.md` for the full detail and the Waveshare wiki references.
