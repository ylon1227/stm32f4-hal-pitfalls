# STM32F4 HAL pitfalls: field notes from a motor-driven embedded system

Real bugs we hit using an **STM32F411** with **STM32Cube HAL** and common peripheral modules (I2C IMU and OLED, SPI microSD, motor drivers), each written up with:

- **Symptoms** (so you can recognise it from what you see),
- **Root cause** (what we actually measured, not guesses),
- **How we diagnosed it** (steps you can repeat),
- **Drop-in fix** (code you can copy).

The issues are general to the STM32F4 family + HAL; the test platform is listed below so you can judge how closely it matches yours.

## Index

| Folder | Symptom in one line | Status |
|---|---|---|
| [`i2c-busy-recovery/`](i2c-busy-recovery/) | I2C dies when motors start and never recovers; `HAL_BUSY`, BUSY flag stuck, SCL/SDA held low, `DeInit/Init` and slave power-cycle don't help | Root cause found and fixed |
| [`adc-dma-overrun-freeze/`](adc-dma-overrun-freeze/) | ADC + DMA buffer suddenly stops updating forever until hard reset (OVR set, nobody clears it) | Fixed (auto-restart by hardware flags) |
| [`oled-i2c-watchdog-hang/`](oled-i2c-watchdog-hang/) | Blocking I2C to an OLED hangs the whole board; watchdog never fires; NRST doesn't help | Fixed (short timeouts + offline back-off) |

## Test platform

| Part | Model / setting |
|---|---|
| MCU | STM32F411 (Cortex-M4), SYSCLK 100 MHz, PCLK1 50 MHz |
| Toolchain | STM32CubeIDE 1.16.1, GNU Tools for STM32 12.3, STM32F4 HAL |
| I2C bus | I2C1 on PB8 (SCL) / PB9 (SDA), 400 kHz, AF open-drain + internal pull-ups |
| IMU | MPU-6050 module, 3.3 V (external regulator; later switched by a PNP high-side switch on a GPIO), I2C address 0x68 |
| Display | 0.96" 128×64 OLED module, **SSD1315** driver (newer drop-in for SSD1306), I2C ("IIC") interface, module rated 3.3–5 V, run at 3.3 V, address 0x3C, same I2C1 bus as the IMU |
| Storage | "MicroSD Card Adapter" module (common Arduino-type SPI board, 6 pins GND/VCC/MISO/MOSI/SCK/CS): 4.5–5.5 V supply, onboard 3.3 V LDO and level shifter, typ. 80 mA / max 200 mA, microSD ≤ 2 GB and microSDHC ≤ 32 GB. On SPI2 (PB12 CS, PB13 SCK, PB14 MISO, PB15 MOSI), 5 V supplied |
| Motors | 2 × BLDC motor, each with a 450 W-class driver, PWM from TIM3/TIM4 |
| Current sense | WCS1800 Hall current sensors, 5 V |
| Power | battery → buck converters (12 V / 5 V / 3.3 V rails) |

## Who wrote this

A graduate student and an AI assistant (Claude) debugging together; the commit history shows both. Measurements come from logged telemetry on the real hardware. If something here is wrong or you found a better fix, please open an issue.

---

## License

MIT (see [LICENSE](LICENSE))
