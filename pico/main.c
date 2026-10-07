// Pico side: owns the 8x8 WS2812 matrix and its timing. The ESP8285 does WiFi/web.
// UART0 line protocol, see esp/src/main.cpp.
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/uart.h"
#include "ws2812.pio.h"
#include "power.h"
#include "font8x8_basic.h"

#define LED_PIN  22
#define ESP_UART uart0
#define ESP_TX   0
#define ESP_RX   1

// Display layout knobs: identical 8x8 panels side by side, chained DOUT -> DIN.
// Within a panel: LED 0 bottom-right, every row runs the same way.
#define PANELS      2
#define PANEL_W     8
#define W           (PANELS * PANEL_W)
#define H           8
#define SERPENTINE  0  // 1 if every other row runs right->left
#define FLIP_X      1  // 0 if text comes out mirrored within a panel
#define FLIP_Y      1  // 0 if text comes out upside down
#define RIGHT_FIRST 1  // 1: GP22 feeds the rightmost panel; 0 if the panel halves come out swapped

#define MAX_TEXT 200
#define MAX_COLS (W + MAX_TEXT * 9)

static uint32_t pixels[W * H];  // 0xRRGGBB, indexed by LED position on the chain

static int xy(int x, int y) {  // x: 0 = left of the whole display, y: 0 = top
    int panel = x / PANEL_W;
    x %= PANEL_W;
    if (RIGHT_FIRST) panel = PANELS - 1 - panel;  // position on the chain
    if (FLIP_X) x = PANEL_W - 1 - x;
    if (FLIP_Y) y = H - 1 - y;
    if (SERPENTINE && (y & 1)) x = PANEL_W - 1 - x;
    return panel * PANEL_W * H + y * PANEL_W + x;
}

// ponytail: blocking PIO pushes, ~30us/LED (~4ms for 2 panels); move to DMA if this stalls realtime work
static void leds_show(void) {
    static uint32_t frame[W * H];
    power_limit(pixels, frame, W * H);  // array runs off VSYS: never full brightness
    for (int i = 0; i < W * H; i++) {
        uint32_t c = frame[i];
        // WS2812 wants GRB, MSB first; PIO shifts out the top 24 bits
        pio_sm_put_blocking(pio0, 0, (c >> 8 & 0xff) << 24 | (c >> 16 & 0xff) << 16 | (c & 0xff) << 8);
    }
}

// ---- scrolling text ----
static enum { MODE_TEXT, MODE_IMAGE, MODE_CLOCK, MODE_TIMER } mode = MODE_CLOCK;  // boot into the clock ("-- --" until NTP)
static uint8_t cols[MAX_COLS];  // one byte per column, bit y = row y lit
static int ncols, scroll;
static uint32_t text_color, step_ms;
static bool rainbow;
static absolute_time_t next_step;

// Fully saturated hue h (0..1535, red -> yellow -> green -> cyan -> blue -> magenta -> red) at value v.
static uint32_t hue(int h, uint8_t v) {
    uint32_t up = (h & 0xff) * v / 255, dn = v - up;
    switch (h >> 8) {
        case 0:  return v << 16 | up << 8;
        case 1:  return dn << 16 | v << 8;
        case 2:  return v << 8 | up;
        case 3:  return dn << 8 | v;
        case 4:  return up << 16 | v;
        default: return v << 16 | dn;
    }
}

// Render text into columns, proportional: blank glyph edges trimmed, 1-column gap.
// rainbow: hue spread over the string, value = brightest channel of color.
static void text_set(const char *s, uint32_t color, int speed, bool rb) {
    memset(cols, 0, W);  // blank lead-in so text enters from the right and loops cleanly
    ncols = W;
    for (; *s && ncols < MAX_COLS - 9; s++) {
        unsigned char ch = *s < 32 || *s > 126 ? '?' : *s;
        uint8_t gc[8] = {0};
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++)
                if (font8x8_basic[ch][r] >> c & 1) gc[c] |= 1 << r;  // font: LSB = leftmost pixel
        int a = 0, b = 7;
        while (a <= b && !gc[a]) a++;
        while (b >= a && !gc[b]) b--;
        if (a > b) a = 0, b = 2;  // space: 3 blank columns
        for (int c = a; c <= b; c++) cols[ncols++] = gc[c];
        cols[ncols++] = 0;
    }
    text_color = color;
    rainbow = rb;
    step_ms = 1000 / (speed < 1 ? 1 : speed > 60 ? 60 : speed);
    scroll = 0;
    next_step = get_absolute_time();
    mode = MODE_TEXT;
}

static void text_draw(void) {
    for (int x = 0; x < W; x++) {
        int i = (scroll + x) % ncols;
        uint32_t c = text_color;
        if (rainbow && ncols > W) {  // red at the first text column, magenta at the last
            uint8_t v = MAX(text_color >> 16 & 0xff, MAX(text_color >> 8 & 0xff, text_color & 0xff));
            c = hue((i - W) * 1280 / (ncols - W), v);
        }
        for (int y = 0; y < H; y++) pixels[xy(x, y)] = cols[i] >> y & 1 ? c : 0;
    }
    leds_show();
}

// ---- clock and timer ----
// 3x5 digits, one byte per row, bit 2 = leftmost column
static const uint8_t DIGITS[10][5] = {
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
    {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 2, 2, 2}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
};
static int32_t day_secs = -1;        // local seconds since midnight, from the ESP's NTP; -1 = not synced yet
static absolute_time_t day_secs_at;  // when day_secs arrived
static uint32_t clock_color = 0x402000, timer_color;
static absolute_time_t timer_end, alarm_end;
static uint32_t timer_total_s;
static absolute_time_t next_face;

// "AA BB" in 3x5 digits (no colon: 16 columns only fit a 2-column gap), plus a bar of `bar` columns on the bottom row.
// a < 0 draws dashes.
static void face_draw(int a, int b, int bar, uint32_t color) {
    memset(pixels, 0, sizeof pixels);
    static const int X0[4] = {0, 4, 9, 13};
    int d[4] = {a / 10, a % 10, b / 10, b % 10};
    for (int i = 0; i < 4; i++)
        for (int r = 0; r < 5; r++)
            for (int c = 0; c < 3; c++)
                if (a < 0 ? r == 2 : DIGITS[d[i]][r] >> (2 - c) & 1) pixels[xy(X0[i] + c, 1 + r)] = color;
    uint32_t dim = color >> 2 & 0x3f3f3f;  // bar at quarter brightness
    for (int x = 0; x < bar && x < W; x++) pixels[xy(x, H - 1)] = dim;
    leds_show();
}

static void clock_draw(void) {
    if (day_secs < 0) {
        face_draw(-1, 0, 0, clock_color);
        return;
    }
    int s = (day_secs + absolute_time_diff_us(day_secs_at, get_absolute_time()) / 1000000) % 86400;
    face_draw(s / 3600, s / 60 % 60, (s % 60 + 1) * W / 60, clock_color);  // bar fills over the minute
}

static void timer_draw(void) {
    int64_t left_us = absolute_time_diff_us(get_absolute_time(), timer_end);
    if (left_us > 0) {
        int s = (left_us + 999999) / 1000000;  // round up: shows 00:01 until it really is over
        int bar = (s * W + timer_total_s - 1) / timer_total_s;  // remaining fraction
        if (s >= 3600) face_draw(s / 3600, s / 60 % 60, bar, timer_color);  // H MM
        else face_draw(s / 60, s % 60, bar, timer_color);                     // MM SS
        return;
    }
    if (is_nil_time(alarm_end)) alarm_end = make_timeout_time_ms(10000);
    if (time_reached(alarm_end)) {  // alarm done: back to the clock
        mode = MODE_CLOCK;
        clock_draw();
        return;
    }
    bool on = to_ms_since_boot(get_absolute_time()) / 250 % 2;  // flash 2x per second
    for (int i = 0; i < W * H; i++) pixels[i] = on ? timer_color : 0;
    leds_show();
}

static void face_now(void) {
    next_face = get_absolute_time();
}

// ---- UART RX into a ring buffer from the IRQ, so the main loop never misses bytes ----
#define RX_SIZE 1024  // power of two
static volatile char rx_buf[RX_SIZE];
static volatile uint16_t rx_head, rx_tail;

static void on_uart_rx(void) {
    while (uart_is_readable(ESP_UART)) {
        char c = uart_getc(ESP_UART);
        uint16_t next = (rx_head + 1) & (RX_SIZE - 1);
        if (next != rx_tail) {  // drop on overflow
            rx_buf[rx_head] = c;
            rx_head = next;
        }
    }
}

// Returns a complete line (without '\n') or NULL. Non-blocking.
static char *read_line(void) {
    static char line[W * H * 6 + 16];  // fits a full IMG line
    static size_t n;
    while (rx_tail != rx_head) {
        char c = rx_buf[rx_tail];
        rx_tail = (rx_tail + 1) & (RX_SIZE - 1);
        if (c == '\r') continue;
        if (c != '\n') {
            if (n < sizeof line - 1) line[n++] = c;
            continue;
        }
        line[n] = 0;
        n = 0;
        return line;
    }
    return NULL;
}

static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// IMG payload: W*H pixels as rrggbb hex, row-major from the top-left of the whole display.
static bool image_set(const char *h) {
    if (strlen(h) != W * H * 6) return false;
    for (int i = 0; i < W * H * 6; i++)
        if (hexval(h[i]) < 0) return false;
    for (int i = 0; i < W * H; i++) {
        uint32_t c = 0;
        for (int k = 0; k < 6; k++) c = c << 4 | hexval(h[i * 6 + k]);
        pixels[xy(i % W, i / W)] = c;
    }
    mode = MODE_IMAGE;
    leds_show();
    return true;
}

static void handle(char *line) {
    static int wifi = -1;
    int up, speed, rb, off = -1;
    unsigned c;
    char ip[16];
    if (sscanf(line, "WIFI %d %15s", &up, ip) == 2) {
        if (up == wifi) return;
        wifi = up;
        printf("wifi %s %s\n", up ? "up" : "down", ip);
    } else if (sscanf(line, "TEXT %6x %d %d %n", &c, &speed, &rb, &off) == 3 && off > 0) {
        printf("text #%06x %d/s%s \"%s\"\n", c, speed, rb ? " rainbow" : "", line + off);
        text_set(line + off, c, speed, rb);
    } else if (!strncmp(line, "IMG ", 4)) {
        printf("image %s\n", image_set(line + 4) ? "ok" : "bad");
    } else if (sscanf(line, "TIMER %d %6x", &speed, &c) == 2) {  // seconds, color; 0 cancels
        printf("timer %ds\n", speed);
        if (speed > 0) {
            timer_total_s = speed;
            timer_end = make_timeout_time_ms(speed * 1000);
            timer_color = c;
            alarm_end = nil_time;
            mode = MODE_TIMER;
        } else if (mode == MODE_TIMER) {
            mode = MODE_CLOCK;
        }
        face_now();
    } else if (sscanf(line, "TIME %d", &speed) == 1) {  // every second, aligned to the ESP's second boundary
        day_secs = speed;
        day_secs_at = get_absolute_time();
    } else if (sscanf(line, "CLOCK %6x", &c) == 1) {
        printf("clock #%06x\n", c);
        clock_color = c;
        mode = MODE_CLOCK;
        face_now();
    }
    // anything else (e.g. ESP boot ROM noise) is ignored
}

int main(void) {
    stdio_init_all();

    uart_init(ESP_UART, 115200);
    gpio_set_function(ESP_TX, GPIO_FUNC_UART);
    gpio_set_function(ESP_RX, GPIO_FUNC_UART);
    irq_set_exclusive_handler(UART0_IRQ, on_uart_rx);
    irq_set_enabled(UART0_IRQ, true);
    uart_set_irq_enables(ESP_UART, true, false);

    ws2812_program_init(pio0, 0, pio_add_program(pio0, &ws2812_program), LED_PIN, 800000, false);
    leds_show();  // all off

    absolute_time_t next_tx = get_absolute_time();
    while (true) {
        char *line = read_line();
        if (line) handle(line);
        if (mode == MODE_TEXT && time_reached(next_step)) {
            next_step = delayed_by_ms(next_step, step_ms);
            text_draw();
            scroll = (scroll + 1) % ncols;
        }
        if ((mode == MODE_CLOCK || mode == MODE_TIMER) && time_reached(next_face)) {
            next_face = make_timeout_time_ms(250);
            if (mode == MODE_CLOCK) clock_draw();
            else timer_draw();
        }
        if (time_reached(next_tx)) {  // telemetry, 1 Hz
            next_tx = delayed_by_ms(next_tx, 1000);
            char t[80];
            int n = snprintf(t, sizeof t, "T uptime_s=%lu mode=%s", (unsigned long)(to_ms_since_boot(get_absolute_time()) / 1000),
                             (const char *[]){"text", "image", "clock", "timer"}[mode]);
            if (day_secs >= 0) {
                int s = (day_secs + absolute_time_diff_us(day_secs_at, get_absolute_time()) / 1000000) % 86400;
                n += snprintf(t + n, sizeof t - n, " time=%02d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
            }
            snprintf(t + n, sizeof t - n, "\n");
            uart_puts(ESP_UART, t);
        }
    }
}
