#pragma once
#include <stdint.h>

// Calibration knobs. WS2812 datasheets say ~20mA per channel at 255; real parts vary, measure if you can.
#define LED_MA_PER_CHANNEL 20
// VSYS from USB is ~500mA total; Pico + ESP8285 (TX peaks ~170mA) take the rest.
#define LED_BUDGET_MA      300

// Scale the whole frame (0xRRGGBB) down so estimated draw stays under budget; keeps hue.
static inline void power_limit(const uint32_t *in, uint32_t *out, int n) {
    uint32_t sum = 0;  // max 64*765, no overflow
    for (int i = 0; i < n; i++) sum += (in[i] >> 16 & 0xff) + (in[i] >> 8 & 0xff) + (in[i] & 0xff);
    uint32_t num = LED_BUDGET_MA * 255, den = sum * LED_MA_PER_CHANNEL;
    if (den <= num) num = den = 1;  // under budget: unchanged
    for (int i = 0; i < n; i++) {
        uint32_t r = (in[i] >> 16 & 0xff) * num / den, g = (in[i] >> 8 & 0xff) * num / den, b = (in[i] & 0xff) * num / den;
        out[i] = r << 16 | g << 8 | b;
    }
}
