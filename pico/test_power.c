// Host check: cc test_power.c -o /tmp/t && /tmp/t
#include <assert.h>
#include "power.h"

static uint32_t ma(const uint32_t *p, int n) {
    uint32_t s = 0;
    for (int i = 0; i < n; i++) s += (p[i] >> 16 & 0xff) + (p[i] >> 8 & 0xff) + (p[i] & 0xff);
    return s * LED_MA_PER_CHANNEL / 255;
}

int main(void) {
    uint32_t in[64], out[64];
    for (int i = 0; i < 64; i++) in[i] = 0xffffff;  // full white: ~3.8A
    power_limit(in, out, 64);
    assert(ma(out, 64) <= LED_BUDGET_MA && ma(out, 64) > LED_BUDGET_MA * 9 / 10);

    in[0] = 0x002000;  // single dim LED: under budget, untouched
    power_limit(in, out, 1);
    assert(out[0] == 0x002000);

    for (int i = 0; i < 64; i++) in[i] = 0xff8000;  // hue kept: r stays ~2x g
    power_limit(in, out, 64);
    assert((out[0] >> 16) >= 2 * (out[0] >> 8 & 0xff) - 1 && (out[0] >> 16) <= 2 * (out[0] >> 8 & 0xff) + 1);
    return 0;
}
