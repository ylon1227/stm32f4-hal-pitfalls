/**
 * i2c_recovery.c — see i2c_recovery.h for the full story.
 * License: MIT
 */
#include "i2c_recovery.h"

/* ~5 us at 100 MHz; adjust for your core clock. Only used during recovery. */
#ifndef I2C_REC_DELAY_LOOPS
#define I2C_REC_DELAY_LOOPS 500u
#endif

static void rec_delay(void)
{
    for (volatile uint32_t d = 0; d < I2C_REC_DELAY_LOOPS; d++) { }
}

static void rcc_reset_instance(I2C_TypeDef *inst)
{
    if (inst == I2C1) { __HAL_RCC_I2C1_FORCE_RESET(); rec_delay(); __HAL_RCC_I2C1_RELEASE_RESET(); }
#if defined(I2C2)
    else if (inst == I2C2) { __HAL_RCC_I2C2_FORCE_RESET(); rec_delay(); __HAL_RCC_I2C2_RELEASE_RESET(); }
#endif
#if defined(I2C3)
    else if (inst == I2C3) { __HAL_RCC_I2C3_FORCE_RESET(); rec_delay(); __HAL_RCC_I2C3_RELEASE_RESET(); }
#endif
}

static void pins_input(const i2c_rec_cfg_t *c)
{
    GPIO_InitTypeDef g = {0};
    g.Mode = GPIO_MODE_INPUT;      /* NOT analog: analog mode reads IDR as 0 */
    g.Pull = GPIO_NOPULL;
    g.Pin  = c->scl_pin; HAL_GPIO_Init(c->scl_port, &g);
    g.Pin  = c->sda_pin; HAL_GPIO_Init(c->sda_port, &g);
}

static void reinit(I2C_HandleTypeDef *h, const i2c_rec_cfg_t *c)
{
    HAL_I2C_Init(h);                                   /* MspInit puts pins back to AF */
    (void)HAL_I2CEx_ConfigAnalogFilter(h, I2C_ANALOGFILTER_ENABLE);
    (void)HAL_I2CEx_ConfigDigitalFilter(h, c->digital_filter);   /* Init cleared FLTR */
}

i2c_rec_holder_t i2c_rec_who_holds(I2C_HandleTypeDef *h, const i2c_rec_cfg_t *c)
{
    if (!i2c_rec_busy(h) &&
        HAL_GPIO_ReadPin(c->scl_port, c->scl_pin) == GPIO_PIN_SET &&
        HAL_GPIO_ReadPin(c->sda_port, c->sda_pin) == GPIO_PIN_SET) {
        return I2C_REC_FREE;
    }
    HAL_I2C_DeInit(h);
    pins_input(c);
    for (int k = 0; k < 4; k++) rec_delay();            /* let the pull-ups win */
    uint8_t scl_lo = (HAL_GPIO_ReadPin(c->scl_port, c->scl_pin) == GPIO_PIN_RESET);
    uint8_t sda_lo = (HAL_GPIO_ReadPin(c->sda_port, c->sda_pin) == GPIO_PIN_RESET);
    rcc_reset_instance(h->Instance);
    reinit(h, c);
    return (scl_lo || sda_lo) ? I2C_REC_EXT_HOLDING : I2C_REC_MCU_HOLDING;
}

uint8_t i2c_rec_full_reset(I2C_HandleTypeDef *h, const i2c_rec_cfg_t *c)
{
    GPIO_InitTypeDef g = {0};

    /* 1-3: peripheral off, pins disconnected, hard reset of the IP */
    HAL_I2C_DeInit(h);
    rcc_reset_instance(h->Instance);

    pins_input(c);
    uint8_t sda_before = (HAL_GPIO_ReadPin(c->sda_port, c->sda_pin) == GPIO_PIN_RESET);

    /* 4: 9 SCL clocks as open-drain GPIO, then a manual STOP */
    g.Mode  = GPIO_MODE_OUTPUT_OD;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    g.Pin   = c->scl_pin; HAL_GPIO_Init(c->scl_port, &g);
    for (int i = 0; i < 9; i++) {
        HAL_GPIO_WritePin(c->scl_port, c->scl_pin, GPIO_PIN_RESET); rec_delay();
        HAL_GPIO_WritePin(c->scl_port, c->scl_pin, GPIO_PIN_SET);   rec_delay();
    }
    g.Pin = c->sda_pin; HAL_GPIO_Init(c->sda_port, &g);
    HAL_GPIO_WritePin(c->scl_port, c->scl_pin, GPIO_PIN_RESET); rec_delay();
    HAL_GPIO_WritePin(c->sda_port, c->sda_pin, GPIO_PIN_RESET); rec_delay();
    HAL_GPIO_WritePin(c->scl_port, c->scl_pin, GPIO_PIN_SET);   rec_delay();
    HAL_GPIO_WritePin(c->sda_port, c->sda_pin, GPIO_PIN_SET);   rec_delay();   /* STOP */

    pins_input(c);
    uint8_t sda_after = (HAL_GPIO_ReadPin(c->sda_port, c->sda_pin) == GPIO_PIN_RESET);

    /* 5: back to AF + filters */
    reinit(h, c);
    return (uint8_t)((sda_before ? 1u : 0u) | (sda_after ? 2u : 0u));
}
