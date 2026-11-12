# Blocking I2C to an OLED hangs the whole board, and the watchdog never fires

## Symptoms

- A 128×64 I2C OLED (SSD1315 here, SSD1306-compatible) is refreshed from the main loop with blocking `HAL_I2C_Mem_Write`.
- Under electrical noise (motors/PWM running) the system **freezes**: control loop effectively stops, UART telemetry stops, but there is **no HardFault** and **the independent watchdog (IWDG) never resets the chip**.
- Pressing **NRST does not fix it** (the display controller is not reset by the MCU's reset pin); only a **power cycle** does.
- Post-mortem breadcrumbs (we record the "current stage" of the main loop in a variable that survives reset and print it on the next boot) show the last stage was the **OLED refresh**, fault = none, reset cause = NRST pin.

## Root cause: three things lining up

1. **The I2C bus gets stuck** after a corrupted transaction (SDA/SCL held low, BUSY set). See [`../i2c-busy-recovery/`](../i2c-busy-recovery/): in our later investigation the line holder turned out to be the **MCU's own I2C peripheral**, which a plain `DeInit/Init` does not release.
2. **Every blocking HAL call then waits for its timeout.** With the original timeouts (20 ms per command byte, 100 ms per 128-byte page), one full-screen flush = 8 pages × (3 × 20 ms + 100 ms) ≈ **1.28 s**. Refreshing at 2 Hz means the main loop spends almost **100 % of its time** inside the OLED code. The 1 kHz control loop dropped to about 0.8 Hz; integrators that use the measured `dt` then received `dt ≈ 1.3 s` and diverged.
3. **The watchdog was fed in the outer loop**, which still ran roughly every 1.3 s, **less than the 4 s IWDG timeout**. So the watchdog was happy while the system was functionally dead. Permanent hang.

## Fix

**1. Short, realistic timeouts.** At 400 kHz a 2-byte command takes ~70 µs and a 128-byte page ~3 ms. Use 5 ms / 10 ms, not 20 / 100 ms.

**2. Take the display offline after repeated failures, skip I2C entirely, probe rarely.**

```c
#define OLED_FAIL_LIMIT  3u
#define OLED_RETRY_MS    5000u
static uint8_t  oled_online = 1, oled_fails = 0;
static uint32_t oled_retry_at = 0;

static void oled_note(HAL_StatusTypeDef st)
{
    if (st == HAL_OK) { oled_fails = 0; oled_online = 1; return; }
    if (oled_fails < OLED_FAIL_LIMIT) oled_fails++;
    if (oled_fails >= OLED_FAIL_LIMIT && oled_online) {
        oled_online = 0;
        oled_retry_at = HAL_GetTick() + OLED_RETRY_MS;
    }
}

static int oled_gate(void)            /* 1 = allowed to touch I2C this time */
{
    if (oled_online) return 1;
    if ((int32_t)(HAL_GetTick() - oled_retry_at) < 0) return 0;
    oled_retry_at = HAL_GetTick() + OLED_RETRY_MS;
    oled_fails = 0;
    return 1;                          /* one probe every 5 s */
}
```

Wrap every write with `oled_note(...)` and start every flush with `if (!oled_gate()) return;`. The failure mode degrades from "whole board dead, pull the power" to "screen blank, everything else runs".

**3. Check BUSY yourself before calling HAL.** HAL first waits up to 25 ms for the BUSY flag (`I2C_TIMEOUT_BUSY_FLAG`) regardless of your timeout argument:

```c
if (__HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_BUSY) == SET) return HAL_BUSY;
```

**4. Recover the bus properly** (RCC reset of the I2C peripheral, not just `DeInit/Init`): see [`../i2c-busy-recovery/`](../i2c-busy-recovery/).

## Two traps we fell into

- **"State != READY then DeInit/Init" never runs.** After a failed HAL call, `hi2c->State` is already back to `HAL_I2C_STATE_READY`, so a recovery guarded by that condition is dead code.
- **Making the OLED depend on another device's health.** We briefly made the OLED skip probing while the IMU (same bus) was offline; when the IMU stayed down, the screen never came back. Keep each device's back-off independent and let one module own bus recovery.

## Recommendation on the watchdog (lesson, not yet applied in our code)

Feeding the IWDG from the outer loop only proves the loop is not *completely* stuck. Feed it only when the **control tick actually ran recently** (e.g. a counter incremented in the 1 ms tick, checked before refreshing), so a loop that crawls at 1 Hz also gets reset.

## Test platform

See the [top-level README](../README.md#test-platform). 0.96" 128×64 SSD1315 I2C OLED at 0x3C, 3.3 V, I2C1 400 kHz shared with an MPU-6050. IWDG prescaler 64, reload 2000 (4 s).

Keywords: STM32 I2C OLED freeze, SSD1306 SSD1315 hang, HAL_I2C_Mem_Write timeout blocks main loop, watchdog not resetting, IWDG fed but system hung, NRST does not reset display, I2C display EMI motor

## License

MIT (see [LICENSE](../LICENSE))
