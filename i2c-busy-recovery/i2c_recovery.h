/**
 * i2c_recovery.h, Runtime recovery for a stuck STM32F4 I2C master (HAL)
 *
 * Problem this solves
 *   On STM32F4 (tested on STM32F411, I2C1 @ 400 kHz, MPU-6050 + SSD1315 on the bus),
 *   motor/PWM noise can leave the *MCU's own* I2C peripheral stuck in master mode,
 *   holding SCL and/or SDA low with the BUSY flag set. After that:
 *     - every HAL_I2C_* call first waits I2C_TIMEOUT_BUSY_FLAG (25 ms) for BUSY,
 *       regardless of the Timeout argument, then returns HAL_BUSY;
 *     - HAL_I2C_DeInit + HAL_I2C_Init (which pulses SWRST) does NOT release it;
 *     - power-cycling the slave does NOT help (the master is the one holding the lines);
 *     - only a full board power cycle helped, until the RCC peripheral reset below.
 *
 * What works
 *   1. HAL_I2C_DeInit()                       (PE off, clock off, pins de-initialised)
 *   2. pins -> plain GPIO                      (disconnect them from the peripheral)
 *   3. RCC force/release reset of the I2Cx     (much stronger than SWRST)
 *   4. 9 SCL clocks + manual STOP as GPIO      (frees a slave stuck mid-byte)
 *   5. HAL_I2C_Init() + re-apply filters        (Init clears FLTR)
 *
 * How to tell who is holding the bus
 *   Reading the pins while they are still in AF mode cannot tell master from slave.
 *   Switch them to *input* (NOT analog: HAL_I2C_DeInit leaves them analog and IDR then
 *   always reads 0), wait a few microseconds, then read:
 *     high  -> the MCU peripheral was holding them  -> full reset fixes it
 *     low   -> a slave/wiring is holding them       -> 9 clocks; else power-cycle the slave
 *
 * License: MIT (see LICENSE)
 */
#ifndef I2C_RECOVERY_H
#define I2C_RECOVERY_H

#include "stm32f4xx_hal.h"
#include <stdint.h>

typedef struct {
    GPIO_TypeDef *scl_port; uint16_t scl_pin;
    GPIO_TypeDef *sda_port; uint16_t sda_pin;
    uint32_t      digital_filter;   /* 0..15, re-applied after every Init (0 = off) */
} i2c_rec_cfg_t;

typedef enum {
    I2C_REC_FREE = 0,       /* both lines high, BUSY clear                  */
    I2C_REC_MCU_HOLDING,    /* lines go high once disconnected from the IP  */
    I2C_REC_EXT_HOLDING     /* still low when disconnected: slave or wiring */
} i2c_rec_holder_t;

/* 1 if the peripheral reports BUSY. Check this BEFORE calling HAL_I2C_* to avoid
 * HAL's fixed 25 ms BUSY wait. */
static inline uint8_t i2c_rec_busy(I2C_HandleTypeDef *h)
{
    return (__HAL_I2C_GET_FLAG(h, I2C_FLAG_BUSY) == SET) ? 1u : 0u;
}

/* Diagnose who holds the bus. Leaves the peripheral re-initialised. */
i2c_rec_holder_t i2c_rec_who_holds(I2C_HandleTypeDef *h, const i2c_rec_cfg_t *c);

/* Full recovery (steps 1–5 above). ~0.1–0.5 ms, no HAL_Delay, no printf.
 * Returns bit0 = SDA was low before, bit1 = SDA still low after. */
uint8_t i2c_rec_full_reset(I2C_HandleTypeDef *h, const i2c_rec_cfg_t *c);

#endif /* I2C_RECOVERY_H */
