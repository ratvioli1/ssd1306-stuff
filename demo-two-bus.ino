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
#define MUX_B21                                                                                    \
    0x0300115CUL /* XGPIOB[21] pad function (bus B SDA)                                            \
                  */
#define MUX_B20                                                                                    \
    0x03001158UL /* XGPIOB[20] pad function (bus B SCL)                                            \
                  */
#define GPIOA_BASE 0x03020000UL
#define GPIOB_BASE 0x03021000UL
#define SDA_A 11
#define SCL_A 12
#define SDA_B 21
#define SCL_B 20
#define FUNC_GPIO 3u

#define DR (GPIOB_BASE + 0x00)
#define DDR (GPIOB_BASE + 0x04)
#define EXT (GPIOB_BASE + 0x50)

typedef struct {
    int sda;
    int scl;
} i2c_bus_t;

static i2c_bus_t bus_a = {SDA_A, SCL_A};
static i2c_bus_t bus_b = {SDA_B, SCL_B};

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

static void sda_low(const i2c_bus_t* bus) {
    pad_out(bus->sda, 0);
}

static void sda_rel(const i2c_bus_t* bus) {
    pad_rel(bus->sda);
}

static void scl_low(const i2c_bus_t* bus) {
    pad_out(bus->scl, 0);
}

static void scl_rel(const i2c_bus_t* bus) {
    pad_rel(bus->scl);
}

static int sda_val(const i2c_bus_t* bus) {
    pad_rel(bus->sda);
    return (int)((r32(EXT) >> bus->sda) & 1u);
}

static void half(void) {
    delayMicroseconds(5);
} /* ~70 kHz */

static void i2c_start(const i2c_bus_t* bus) {
    sda_rel(bus);
    scl_rel(bus);
    half();
    sda_low(bus);
    half();
    scl_low(bus);
    half();
}

static void i2c_stop(const i2c_bus_t* bus) {
    sda_low(bus);
    half();
    scl_rel(bus);
    half();
    sda_rel(bus);
    half();
}

static void wbit(const i2c_bus_t* bus, int b) {
    if (b)
        sda_rel(bus);
    else
        sda_low(bus);
    half();
    scl_rel(bus);
    half();
    scl_low(bus);
    half();
}

static int rbit(const i2c_bus_t* bus) {
    sda_rel(bus);
    half();
    scl_rel(bus);
    half();
    int v = sda_val(bus);
    scl_low(bus);
    half();
    return v;
}

static int wbyte(const i2c_bus_t* bus, uint8_t v) {
    for (int i = 7; i >= 0; i--)
        wbit(bus, (v >> i) & 1);
    return rbit(bus); /* 0 = ACK */
}

static int wbuf(const i2c_bus_t* bus, const uint8_t* b, size_t n) {
    i2c_start(bus);
    if (wbyte(bus, (uint8_t)(OLED_ADDR << 1)) != 0) {
        i2c_stop(bus);
        return -1;
    }
    for (size_t i = 0; i < n; i++)
        (void)wbyte(bus, b[i]);
    i2c_stop(bus);
    return 0;
}

/* transport callback expected by ssd1306.h */
static void oled_cb(void* ctx, const uint8_t* buf, size_t len, int is_data) {
    const i2c_bus_t* bus = (const i2c_bus_t*)ctx;
    uint8_t tmp[1 + 64];
    for (size_t off = 0; off < len; off += 64) {
        size_t n = len - off;
        if (n > 64)
            n = 64;
        tmp[0] = is_data ? 0x40 : 0x00;
        memcpy(tmp + 1, buf + off, n);
        (void)wbuf(bus, tmp, n + 1);
    }
}

static inline int get_a28() {
    return (r32(GPIOA_BASE + 0x50) >> 28) & 1u;
}

static oled_t o1;
static oled_t o2;

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
    w32(MUX_B21, (r32(MUX_B21) & ~0x7u) | FUNC_GPIO);
    w32(MUX_B20, (r32(MUX_B20) & ~0x7u) | FUNC_GPIO);

    sda_rel(&bus_a);
    scl_rel(&bus_a);
    sda_rel(&bus_b);
    scl_rel(&bus_b);

    oled_init(&o1, oled_cb, &bus_a);
    oled_init(&o2, oled_cb, &bus_b);
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
