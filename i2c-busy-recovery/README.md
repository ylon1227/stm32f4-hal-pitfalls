# STM32F4 I2C stuck BUSY / SCL SDA held low — runtime recovery (HAL)

**Symptoms this fixes** (STM32F4, HAL, tested on STM32F411 + MPU-6050 + SSD1315 OLED, 400 kHz):

- I2C works, then **dies as soon as a motor / PWM load starts** and **never comes back**, even after the motor stops.
- `HAL_I2C_Mem_Read` / `HAL_I2C_Master_Transmit` return `HAL_BUSY` or `HAL_ERROR`; the `BUSY` flag in `SR2` stays set.
- Every failed call blocks **~25 ms** even with `Timeout = 1..3 ms` (HAL waits `I2C_TIMEOUT_BUSY_FLAG` first).
- `HAL_I2C_DeInit()` + `HAL_I2C_Init()` (SWRST) **does not help**. Power-cycling the slave **does not help**. Pressing MCU reset alone may not help. **Only a full power cycle** restores it.
- Bit-banging 9 SCL clocks alone does not help.

## Root cause (what we measured)

The **MCU's own I2C peripheral** was holding SCL and SDA low. We proved it by switching PB8/PB9 from AF to **plain input** while the bus was "stuck": both lines immediately went high. With the pins connected to the peripheral they read low.

> Gotcha: after `HAL_I2C_DeInit()` the pins are in **analog** mode, where `IDR` always reads 0. You must set them to **input** before reading, otherwise you will wrongly conclude "the slave is holding the bus".

## Fix

```c
HAL_I2C_DeInit(&hi2c1);                 // PE off, clock off
__HAL_RCC_I2C1_FORCE_RESET();           // hard reset of the IP (stronger than SWRST)
__HAL_RCC_I2C1_RELEASE_RESET();
// 9 SCL clocks + STOP as open-drain GPIO (frees a slave stuck mid-byte)
HAL_I2C_Init(&hi2c1);                   // MspInit restores AF
HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE);
HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 4);   // Init clears FLTR, re-apply
```

`i2c_recovery.c/.h` in this repo wraps this as `i2c_rec_full_reset()` and adds `i2c_rec_who_holds()` (MCU vs external). ~0.1–0.5 ms, no `HAL_Delay`, no `printf`, safe inside a 1 kHz control loop.

### Recommended read pattern

```c
static const i2c_rec_cfg_t cfg = { GPIOB, GPIO_PIN_8, GPIOB, GPIO_PIN_9, 4 };

uint8_t sensor_read(uint8_t *buf)
{
    if (i2c_rec_busy(&hi2c1)) {                 // never enter HAL while BUSY (25 ms stall)
        i2c_rec_full_reset(&hi2c1, &cfg);
        return 0;                               // retry next cycle
    }
    if (HAL_I2C_Mem_Read(&hi2c1, ADDR, REG, I2C_MEMADD_SIZE_8BIT, buf, 14, 3) != HAL_OK) {
        i2c_rec_full_reset(&hi2c1, &cfg);       // recover immediately, retry next cycle
        return 0;
    }
    return 1;
}
```

> Note: the 70-recovery result below was measured with the reset done after a slower wake/config sequence. The immediate-reset pattern above is what we run now; its numbers will be added after testing.

If several consecutive resets do not help, check `i2c_rec_who_holds()`: `I2C_REC_EXT_HOLDING` means the slave or wiring holds the line → power-cycle the slave (we use a PNP high-side switch on its VCC driven by a GPIO, default-on with a base pull-down).

## Results

| Firmware | Recovers after motor-induced failure? |
|---|---|
| DeInit/Init (SWRST) only | No, never |
| + slave power-cycle | No |
| + RCC reset, but pins read while still in AF | No (couldn't see the real cause) |
| **+ pins switched to input, RCC reset, re-init** | **Yes** – 70 automatic recoveries in one run, longest outage 1.48 s |

Also important: checking `BUSY` before calling HAL removed the 25 ms stalls, so the control loop no longer collapses (it dropped to ~29 % duty before).

## Debugging approach that finally worked

1. Confirm it is not a timing issue (loop period before the failure was a clean 1 ms).
2. **Add observability first**: we sent a step code in our telemetry packet on every failure (which recovery step failed, BUSY state, SCL/SDA levels). Guessing without it cost us a whole evening.
3. Read the real pin levels **with the pins disconnected from the peripheral**.
4. Remove other bus devices / noisy neighbours one at a time (OLED, SD card) — they were not the cause here.

## Related reports

- ST Community: [How to reset I2C on STM32F407 without resetting the MCU](https://community.st.com/stm32-mcus-products-25/how-to-reset-i2c-on-stm32f4407-without-resetting-the-mcu-83642)
- ST Community: [I2C clock line held low by STM32 and not recovered](https://community.st.com/stm32-mcus-products-25/i2c-clock-line-held-low-by-stm32-and-not-recovered-126919)
- ST Community: [Can't reset I2C in STM32F407 to release I2C lines](https://community.st.com/s/question/0D50X00009Xkeul/cant-reset-i2c-in-stm32f407-to-release-i2c-lines?)
- ST errata (F1 family): "I2C analog filter may provide wrong value, locking BUSY flag" ([ES0348 §2.8.7](https://www.st.com.cn/resource/en/errata_sheet/es0348-stm32f101x46-stm32f102x46-stm32f103x46-device-errata-stmicroelectronics.pdf)). We have not verified whether the F411 errata (ES0287) lists the same item.

Keywords: STM32F4 STM32F411 STM32F407 I2C BUSY flag stuck, HAL_BUSY, SCL held low, SDA held low, I2C lockup, bus recovery, RCC reset, SWRST not working, MPU6050 stops when motor runs, I2C EMI motor PWM, HAL_I2C_Mem_Read timeout 25 ms


## License

MIT
