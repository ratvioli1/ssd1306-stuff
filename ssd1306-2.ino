/* oled-text.ino - drive the SSD1306 from the little C906 core with no OS,
 * no Wire library, and no DesignWare controller: bit-bang I2C on the header
 * pads and reuse the same transport-agnostic ssd1306.h as the Linux examples.
 *
 * VERIFIED on the Milk-V Duo S via remoteproc. Header pin 11 = XGPIOB[11] =
 * IIC1_SDA, pin 13 = XGPIOB[12] = IIC1_SCL. The little core runs in M-mode,
 * so the register addresses below are physical: no /dev/mem, no mmap.
 *
 * NOTE: this bit-bang path does NOT use the kernel I2C controller; it
 * reprograms the B11/B12 pad-mux registers as plain GPIO itself.
 *
 * Build: arduino-cli compile --fqbn sophgo:SG200X:duos --build-path build
 * oled-text
 */
#include <string.h>

#include "images.h"
#include "ssd1306.h"

#define MUX_B11                                                                                    \
    0x03001134UL /* XGPIOB[11] pad function (header pin 11, SDA)                                   \
                  */
#define MUX_B12                                                                                    \
    0x03001138UL /* XGPIOB[12] pad function (header pin 13, SCL)                                   \
                  */
#define MUX_B13                                                                                    \
    0x0300113CUL /* XGPIOB[12] pad function (header pin 13, SCL)                                   \
                  */
#define GPIOA_BASE 0x03020000UL
#define GPIOB_BASE 0x03021000UL
#define SDA 11
#define SCL_A 12
#define SCL_B 13
#define FUNC_GPIO 3u

#define DR (GPIOB_BASE + 0x00)
#define DDR (GPIOB_BASE + 0x04)
#define EXT (GPIOB_BASE + 0x50)

static int active_scl = SCL_A;

static inline uint32_t r32(uint32_t a) {
    return *(volatile uint32_t*)a;
}

static inline void w32(uint32_t a, uint32_t v) {
    *(volatile uint32_t*)a = v;
}

/* open-drain emulation: drive low as output, release (high-Z) for high */
static void pad_out(int bit, int high) {
    uint32_t d = r32(DR);
    if (high)
        d |= (1u << bit);
    else
        d &= ~(1u << bit);
    w32(DR, d);
    w32(DDR, r32(DDR) | (1u << bit));
}

static void pad_rel(int bit) {
    w32(DDR, r32(DDR) & ~(1u << bit));
}

static void sda_low(void) {
    pad_out(SDA, 0);
}

static void sda_rel(void) {
    pad_rel(SDA);
}

static void scl_low(void) {
    pad_out(active_scl, 0);
}

static void scl_rel(void) {
    pad_rel(active_scl);
}

static int sda_val(void) {
    pad_rel(SDA);
    return (int)((r32(EXT) >> SDA) & 1u);
}

static void half(void) {
    delayMicroseconds(5);
} /* ~70 kHz */

static void i2c_start(void) {
    sda_rel();
    scl_rel();
    half();
    sda_low();
    half();
    scl_low();
    half();
}

static void i2c_stop(void) {
    sda_low();
    half();
    scl_rel();
    half();
    sda_rel();
    half();
    scl_low();
}

static void wbit(int b) {
    if (b)
        sda_rel();
    else
        sda_low();
    half();
    scl_rel();
    half();
    scl_low();
    half();
}

static int rbit(void) {
    sda_rel();
    half();
    scl_rel();
    half();
    int v = sda_val();
    scl_low();
    half();
    return v;
}

static int wbyte(uint8_t v) {
    for (int i = 7; i >= 0; i--)
        wbit((v >> i) & 1);
    return rbit(); /* 0 = ACK */
}

static int wbuf(const uint8_t* b, size_t n) {
    i2c_start();
    if (wbyte((uint8_t)(OLED_ADDR << 1)) != 0) {
        i2c_stop();
        return -1;
    }
    for (size_t i = 0; i < n; i++)
        (void)wbyte(b[i]);
    i2c_stop();
    return 0;
}

/* transport callback expected by ssd1306.h */
static void oled_cb(void* ctx, const uint8_t* buf, size_t len, int is_data) {
    active_scl = *(const uint8_t*)ctx;  // yuck
    uint8_t tmp[1 + 64];
    for (size_t off = 0; off < len; off += 64) {
        size_t n = len - off;
        if (n > 64)
            n = 64;
        tmp[0] = is_data ? 0x40 : 0x00;
        memcpy(tmp + 1, buf + off, n);
        (void)wbuf(tmp, n + 1);
    }
}

static inline int get_a28() {
    return (r32(GPIOA_BASE + 0x50) >> 28) & 1u;
}

static oled_t o1;
static oled_t o2;
static uint8_t o1_scl = SCL_A;
static uint8_t o2_scl = SCL_B;

int get_pixel_from_img(const uint8_t* data, int x, int y, int img_width, int img_height) {
    int row = y * (img_width / 8);
    int col = x / 8;
    // int bit = aaaa[row + col] >> colBit;
    int bit = data[row + col] & (0b10000000 >> (x % 8));
    return bit != 0;
}

void draw_image(oled_t* o, const uint8_t* data, int width, int height, int startX, int startY) {
    for (int y = 0; y < height; y++) {
        int screenY = startY + y;

        if (screenY < 0 || screenY >= OLED_H)
            continue;

        for (int x = 0; x < width; x++) {
            int screenX = startX + x;

            if (screenX < 0 || screenX >= OLED_W)
                continue;

            if (get_pixel_from_img(data, x, y, width, height))
                oled_pixel(o, screenX, screenY);
        }
    }
}

void draw_smiley(oled_t* o, int startX, int startY) {
    draw_image(o, smiley, SMILEY_WIDTH, SMILEY_HEIGHT, startX, startY);
}

void draw_frowny(oled_t* o, int startX, int startY) {
    draw_image(o, frowny, FROWNY_WIDTH, FROWNY_HEIGHT, startX, startY);
}

void setup() {
    w32(MUX_B11, (r32(MUX_B11) & ~0x7u) | FUNC_GPIO);
    w32(MUX_B12, (r32(MUX_B12) & ~0x7u) | FUNC_GPIO);
    w32(MUX_B13, (r32(MUX_B13) & ~0x7u) | FUNC_GPIO);
    sda_rel();

    // scl_rel(); // can't be used since it only sets one of them
    pad_out(SCL_A, 0);
    pad_out(SCL_B, 0);

    oled_init(&o1, oled_cb, &o1_scl);
    oled_init(&o2, oled_cb, &o2_scl);
    // oled_text(&o, 0, 0, MESSAGE);
    // oled_text(&o, 0, 12, "SSD1306 I2C");

    draw_smiley(&o1, 32, 16);
    oled_flush(&o1);

    draw_frowny(&o2, 32, 16);
    oled_flush(&o2);
}

void loop() {
    static int previous = -1;
    int current = get_a28();

    if (current != previous) {
        if (current != 0) {
            oled_clear(&o1);
            draw_smiley(&o1, 32, 16);
            oled_flush(&o1);

            oled_clear(&o2);
            draw_frowny(&o2, 32, 16);
            oled_flush(&o2);
        } else {
            oled_clear(&o2);
            draw_smiley(&o2, 32, 16);
            oled_flush(&o2);

            oled_clear(&o1);
            draw_frowny(&o1, 32, 16);
            oled_flush(&o1);
        }

        previous = current;
    }

    delay(10);
}
