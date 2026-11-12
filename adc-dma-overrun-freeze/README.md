# STM32F4 ADC + DMA values freeze forever (OVR set, nobody clears it)

## Symptoms

- ADC running in **continuous scan mode with circular DMA** (`HAL_ADC_Start_DMA`), buffer read by the application or by a timer ISR.
- After some time the values in the DMA buffer **stop changing** and stay at the last sample forever. Everything else (UART, timers, main loop) keeps running.
- Only a **hard reset** brings it back. No HardFault, no watchdog reset.
- Anything derived from the ADC (in our case optical encoder edges sampled from the ADC buffer) silently goes wrong: no more edges, speed reads zero, estimators drift.

## Root cause

1. `HAL_ADC_Start_DMA()` enables the **overrun interrupt** (`ADC_IT_OVR`) inside the ADC.
2. But **`ADC_IRQn` was never enabled in the NVIC**, and the **DMA stream IRQ was deliberately disabled** (the DMA2_Stream0 handler is empty) to avoid flooding the CPU with half/full-transfer interrupts from a continuously running scan.
3. If the DMA ever fails to read `ADC_DR` in time, the ADC sets **OVR**. Per the reference manual (RM0383, ADC section), **once OVR is set the ADC stops issuing DMA requests**. With no ISR to clear OVR and restart, the buffer is frozen for good.
4. A **DMA transfer error** has the same end result: hardware clears the stream's `EN` bit and the stream stops.

So the "freeze" is not data corruption; it is a stopped pipeline that nothing restarts.

We did not pin down what makes the DMA miss a conversion in our system (bus contention with other DMA streams and heavy SPI/UART activity are the usual suspects). The point is: **it will eventually happen, and the default HAL setup has no path back.**

## Fix: detect by hardware flags, restart in place

Run this once per control tick (we run it at 1 kHz). It costs two register reads when healthy.

```c
extern ADC_HandleTypeDef hadc1;
extern volatile uint16_t adc_dma_buf[];
volatile uint32_t adc_ovr_recoveries = 0;

void adc_health_check(uint32_t now_ms)
{
    static uint32_t last_recover_ms = 0;
    uint8_t ovr  = __HAL_ADC_GET_FLAG(&hadc1, ADC_FLAG_OVR) ? 1u : 0u;
    uint8_t dead = (DMA2_Stream0->CR & DMA_SxCR_EN) ? 0u : 1u;   /* stream stopped */

    if ((ovr || dead) && (now_ms - last_recover_ms) >= 50u) {    /* 50 ms throttle */
        last_recover_ms = now_ms;

        HAL_ADC_Stop_DMA(&hadc1);
        __HAL_ADC_CLEAR_FLAG(&hadc1, ADC_FLAG_OVR);
        HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_dma_buf, N_CHANNELS);

        /* Wait one full scan so the buffer holds fresh values before
         * anything re-synchronises on it (one scan was ~17 us for us). */
        for (volatile uint32_t k = 0; k < 600; k++) { }

        /* Re-sync anything that depends on the buffer (edge detectors,
         * filters, observers) here, using the fresh values. */

        adc_ovr_recoveries++;   /* expose this in telemetry / on a display */
    }
}
```

Why this design:

- **Detection uses hardware flags, not thresholds** (`OVR`, `DMA_SxCR_EN`), so it never fires on legitimate constant signals.
- **50 ms throttle** so that, if recovery somehow keeps failing, it cannot eat the control loop.
- **Re-sync after restart**: any state machine that compares new samples with old ones (Schmitt triggers, edge counters, observers) must be re-initialised from fresh samples, otherwise the first real edge after recovery is mis-counted.
- **Count recoveries** and show them somewhere. A non-zero counter tells you the problem is real and how often it happens.

## Alternatives

- Enable `ADC_IRQn` in the NVIC and handle OVR in `HAL_ADC_ErrorCallback()` (stop, clear, restart). Cleaner, but adds an ISR at high priority.
- Set ADC DMA priority higher than other streams on the same controller to make OVR less likely. Reduces the chance, does not remove the need for recovery.

## Test platform

See the [top-level README](../README.md#test-platform). ADC1, 6 channels, 12-bit, continuous scan, `DMAContinuousRequests = ENABLE`, DMA2 Stream0 circular, PCLK2/4 ADC clock.

Keywords: STM32F4 ADC DMA stops, ADC values frozen, ADC overrun OVR, DMA circular stops updating, HAL_ADC_Start_DMA freeze, ADC_FLAG_OVR, DMA_SxCR_EN cleared, transfer error, needs reset

## License

MIT (see [LICENSE](../LICENSE))
