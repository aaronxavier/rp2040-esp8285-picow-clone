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
static enum { MODE_IDLE, MODE_TEXT, MODE_IMAGE } mode;
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
        if (mode != MODE_IDLE) return;
        if (up) {
            text_set(ip, 0x00300c, 10, false);  // show where to point the browser
        } else {
            pixels[xy(0, 0)] = 0x200000;  // red dot: no WiFi yet
            leds_show();
        }
    } else if (sscanf(line, "TEXT %6x %d %d %n", &c, &speed, &rb, &off) == 3 && off > 0) {
        printf("text #%06x %d/s%s \"%s\"\n", c, speed, rb ? " rainbow" : "", line + off);
        text_set(line + off, c, speed, rb);
    } else if (!strncmp(line, "IMG ", 4)) {
        printf("image %s\n", image_set(line + 4) ? "ok" : "bad");
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
        if (time_reached(next_tx)) {  // telemetry, 1 Hz
            next_tx = delayed_by_ms(next_tx, 1000);
            char t[64];
            snprintf(t, sizeof t, "T uptime_s=%lu mode=%s\n", (unsigned long)(to_ms_since_boot(get_absolute_time()) / 1000),
                     mode == MODE_TEXT ? "text" : mode == MODE_IMAGE ? "image" : "idle");
            uart_puts(ESP_UART, t);
        }
    }
}
