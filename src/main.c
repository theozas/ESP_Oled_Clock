/*
   ESP32-C6 + PCA9548A + 5 OLED Digital Clock

   What this code does:
   - Connects ESP32-C6 to Wi-Fi
   - Gets time from internet using NTP
   - Uses PCA9548A I2C multiplexer to control 5 OLED screens
   - Shows one clock digit per OLED
   - Shows blinking colon on middle OLED
   - Shows status messages like CONNECT, SYNC, NO WIFI, NO NET
*/

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdbool.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "driver/i2c.h"

/*
   Wi-Fi setup portal settings.

   If saved Wi-Fi credentials are missing, or the ESP cannot connect,
   it starts this hotspot and serves a simple setup page at 192.168.4.1.
*/
#define SETUP_AP_SSID "CLOCK-SETUP"
#define SETUP_AP_PASS "CLOCKSETUP"

#define WIFI_CONNECT_TIMEOUT_MS 20000

#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_NVS_SSID_KEY "ssid"
#define WIFI_NVS_PASS_KEY "pass"

/*
   I2C settings.

   Your ESP32-C6 wiring:
   SDA = GPIO16
   SCL = GPIO17
*/
#define I2C_PORT I2C_NUM_0
#define I2C_SDA_GPIO 16
#define I2C_SCL_GPIO 17

/*
   I2C speed.

   400 kHz is fast mode.
   Most SSD1306 OLEDs and PCA9548A modules work fine at this speed.
*/
#define I2C_FREQ_HZ 400000

/*
   I2C addresses.

   PCA9548A address is 0x70 because A0, A1, A2 are connected to GND.
   OLED address is usually 0x3C.
*/
#define PCA9548A_ADDR 0x70
#define OLED_ADDR 0x3C

/*
   FreeRTOS event bit used to know when Wi-Fi is connected.
*/
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

/*
   TAG is used by ESP_LOGI / ESP_LOGE messages.
   You see this in the serial monitor.
*/
static const char *TAG = "CLOCK";

/*
   Event group used by Wi-Fi code.
*/
static EventGroupHandle_t wifi_event_group;

/*
   Runtime Wi-Fi state used by the clock loop.
*/
static bool wifi_connected = false;
static int wifi_retry_count = 0;
static bool sntp_started = false;

typedef struct {
    char ssid[33];
    char password[65];
} wifi_credentials_t;

/*
   Screen rotation options.

   Each OLED can be rotated separately.
*/
typedef enum {
    ROT_0,
    ROT_90,
    ROT_180,
    ROT_270
} rotation_t;

/*
   OLED object.

   channel  = PCA9548A channel number
   width    = real OLED width
   height   = real OLED height
   rotation = screen rotation
   buffer   = local screen memory before sending to OLED
*/
typedef struct {
    uint8_t channel;
    uint8_t width;
    uint8_t height;
    rotation_t rotation;
    uint8_t buffer[1024];
} oled_t;

/*
   Your 5 OLED displays.

   Display 0 = hour ones
   Display 1 = hour tens
   Display 2 = colon
   Display 3 = minute tens
   Display 4 = minute ones

   You already adjusted these channels and rotations so the digits appear correctly.
*/
static oled_t displays[5] = {
    { .channel = 1, .width = 128, .height = 64, .rotation = ROT_270 },
    { .channel = 0, .width = 128, .height = 64, .rotation = ROT_270 },
    { .channel = 2, .width = 128, .height = 32, .rotation = ROT_90  },
    { .channel = 3, .width = 128, .height = 64, .rotation = ROT_90  },
    { .channel = 4, .width = 128, .height = 64, .rotation = ROT_90  },
};

/*
   Select one channel on PCA9548A.

   PCA9548A uses one byte:
   bit 0 = channel 0
   bit 1 = channel 1
   bit 2 = channel 2
   etc.

   Example:
   channel 3 means send 00001000.
*/
static esp_err_t pca_select_channel(uint8_t channel)
{
    uint8_t data = 1 << channel;

    esp_err_t err = i2c_master_write_to_device(
        I2C_PORT,
        PCA9548A_ADDR,
        &data,
        1,
        pdMS_TO_TICKS(100)
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCA channel %d select failed", channel);
    }

    return err;
}

/*
   Send one command byte to SSD1306 OLED.

   0x00 means "this byte is a command".
*/
static esp_err_t oled_write_cmd(uint8_t cmd)
{
    uint8_t data[2] = {0x00, cmd};

    return i2c_master_write_to_device(
        I2C_PORT,
        OLED_ADDR,
        data,
        2,
        pdMS_TO_TICKS(100)
    );
}

/*
   Send display pixel data to OLED.

   0x40 means "these bytes are display data".

   Data is sent in small chunks because I2C write buffers
   should not be too large.
*/
static esp_err_t oled_write_data(uint8_t *data, size_t len)
{
    uint8_t temp[17];
    temp[0] = 0x40;

    while (len > 0) {
        size_t chunk = len > 16 ? 16 : len;

        memcpy(&temp[1], data, chunk);

        esp_err_t err = i2c_master_write_to_device(
            I2C_PORT,
            OLED_ADDR,
            temp,
            chunk + 1,
            pdMS_TO_TICKS(100)
        );

        if (err != ESP_OK) {
            return err;
        }

        data += chunk;
        len -= chunk;
    }

    return ESP_OK;
}

/*
   Initialize one OLED screen.

   Because screens are behind the PCA9548A, we must:
   1. Select its channel
   2. Send SSD1306 setup commands
*/
static esp_err_t oled_init(oled_t *d)
{
    esp_err_t err;

    ESP_LOGI(TAG, "Init OLED channel %d", d->channel);

    err = pca_select_channel(d->channel);
    if (err != ESP_OK) return err;

    vTaskDelay(pdMS_TO_TICKS(50));

    /*
       SSD1306 initialization commands.

       These configure:
       - display off/on
       - addressing mode
       - contrast
       - multiplex ratio
       - charge pump
       - COM pins
    */
    uint8_t cmds[] = {
        0xAE,
        0x20, 0x00,
        0xB0,
        0xC8,
        0x00,
        0x10,
        0x40,
        0x81, 0xFF,
        0xA1,
        0xA6,
        0xA8, d->height - 1,
        0xA4,
        0xD3, 0x00,
        0xD5, 0x80,
        0xD9, 0xF1,
        0xDA, d->height == 64 ? 0x12 : 0x02,
        0xDB, 0x40,
        0x8D, 0x14,
        0xAF
    };

    for (int i = 0; i < sizeof(cmds); i++) {
        err = oled_write_cmd(cmds[i]);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OLED init failed on channel %d", d->channel);
            return err;
        }
    }

    ESP_LOGI(TAG, "OLED OK channel %d", d->channel);

    return ESP_OK;
}

/*
   Clear local OLED buffer.

   This does not update the physical screen until oled_display() is called.
*/
static void oled_clear(oled_t *d)
{
    memset(d->buffer, 0, sizeof(d->buffer));
}

/*
   Return logical screen width after rotation.

   If OLED is rotated 90 or 270 degrees,
   width and height are swapped.
*/
static int logical_width(oled_t *d)
{
    if (d->rotation == ROT_90 || d->rotation == ROT_270) {
        return d->height;
    }

    return d->width;
}

/*
   Return logical screen height after rotation.
*/
static int logical_height(oled_t *d)
{
    if (d->rotation == ROT_90 || d->rotation == ROT_270) {
        return d->width;
    }

    return d->height;
}

/*
   Set one pixel in the local buffer.

   This function also handles screen rotation.
   You draw using logical coordinates,
   and this function maps them to real OLED coordinates.
*/
static void oled_set_pixel(oled_t *d, int x, int y, bool on)
{
    int px = x;
    int py = y;

    int lw = logical_width(d);
    int lh = logical_height(d);

    if (x < 0 || y < 0 || x >= lw || y >= lh) {
        return;
    }

    switch (d->rotation) {
        case ROT_0:
            px = x;
            py = y;
            break;

        case ROT_90:
            px = d->width - 1 - y;
            py = x;
            break;

        case ROT_180:
            px = d->width - 1 - x;
            py = d->height - 1 - y;
            break;

        case ROT_270:
            px = y;
            py = d->height - 1 - x;
            break;
    }

    if (px < 0 || py < 0 || px >= d->width || py >= d->height) {
        return;
    }

    /*
       SSD1306 buffer format:
       8 vertical pixels are stored in one byte.
    */
    int index = px + (py / 8) * d->width;
    uint8_t mask = 1 << (py & 7);

    if (on) {
        d->buffer[index] |= mask;
    } else {
        d->buffer[index] &= ~mask;
    }
}

/*
   Draw filled rectangle into local buffer.
*/
static void fill_rect(oled_t *d, int x, int y, int w, int h)
{
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            oled_set_pixel(d, xx, yy, true);
        }
    }
}

/*
   Draw horizontal seven-segment style piece.

   It has angled/beveled ends, so it looks like
   a digital clock segment instead of a plain box.
*/
static void draw_h_segment(oled_t *d, int x, int y, int w, int t)
{
    int bevel = t / 2;
    if (bevel < 2) bevel = 2;

    for (int yy = 0; yy < t; yy++) {
        int cut;

        if (yy < t / 2) {
            cut = bevel - yy;
        } else {
            cut = yy - (t / 2);
        }

        if (cut < 0) cut = 0;
        if (cut > bevel) cut = bevel;

        fill_rect(d, x + cut, y + yy, w - 2 * cut, 1);
    }
}

/*
   Draw right-side vertical segment.

   This is used for upper-right and lower-right digit segments.
*/
static void draw_v_segment_right(oled_t *d, int x, int y, int t, int h)
{
    int bevel = t / 2;
    if (bevel < 2) bevel = 2;

    for (int yy = 0; yy < h; yy++) {
        int cut = 0;

        if (yy < bevel) {
            cut = bevel - yy;
        } else if (yy > h - bevel) {
            cut = yy - (h - bevel);
        }

        if (cut < 0) cut = 0;
        if (cut > bevel) cut = bevel;

        fill_rect(d, x + cut, y + yy, t - cut, 1);
    }
}

/*
   Draw left-side vertical segment.

   This is mirrored compared with the right-side segment.
   This fixed your earlier issue where left segments looked wrong.
*/
static void draw_v_segment_left(oled_t *d, int x, int y, int t, int h)
{
    int bevel = t / 2;
    if (bevel < 2) bevel = 2;

    for (int yy = 0; yy < h; yy++) {
        int cut = 0;

        if (yy < bevel) {
            cut = bevel - yy;
        } else if (yy > h - bevel) {
            cut = yy - (h - bevel);
        }

        if (cut < 0) cut = 0;
        if (cut > bevel) cut = bevel;

        fill_rect(d, x, y + yy, t - cut, 1);
    }
}

/*
   Send local buffer to physical OLED.

   The OLED must be selected through PCA9548A first.
*/
static esp_err_t oled_display(oled_t *d)
{
    esp_err_t err;

    err = pca_select_channel(d->channel);
    if (err != ESP_OK) return err;

    /*
       Set column range.
    */
    oled_write_cmd(0x21);
    oled_write_cmd(0);
    oled_write_cmd(d->width - 1);

    /*
       Set page range.
       One page = 8 vertical pixels.
    */
    oled_write_cmd(0x22);
    oled_write_cmd(0);
    oled_write_cmd((d->height / 8) - 1);

    return oled_write_data(d->buffer, d->width * d->height / 8);
}

/*
   Very small 5x7 font used only for status messages.

   Example:
   CONNECT
   SYNC
   NO WIFI
   NO NET
*/
static const uint8_t *font5x7(char c)
{
    static const uint8_t SPACE[5] = {0,0,0,0,0};
    static const uint8_t DASH[5] = {0x08,0x08,0x08,0x08,0x08};
    static const uint8_t DOT[5] = {0x00,0x60,0x60,0x00,0x00};
    static const uint8_t NUM0[5] = {0x3E,0x45,0x49,0x51,0x3E};
    static const uint8_t NUM1[5] = {0x00,0x21,0x7F,0x01,0x00};
    static const uint8_t NUM2[5] = {0x21,0x43,0x45,0x49,0x31};
    static const uint8_t NUM3[5] = {0x42,0x41,0x51,0x69,0x46};
    static const uint8_t NUM4[5] = {0x0C,0x14,0x24,0x7F,0x04};
    static const uint8_t NUM5[5] = {0x72,0x51,0x51,0x51,0x4E};
    static const uint8_t NUM6[5] = {0x1E,0x29,0x49,0x49,0x06};
    static const uint8_t NUM7[5] = {0x40,0x47,0x48,0x50,0x60};
    static const uint8_t NUM8[5] = {0x36,0x49,0x49,0x49,0x36};
    static const uint8_t NUM9[5] = {0x30,0x49,0x49,0x4A,0x3C};
    static const uint8_t A[5] = {0x7E,0x11,0x11,0x11,0x7E};
    static const uint8_t C[5] = {0x3E,0x41,0x41,0x41,0x22};
    static const uint8_t D[5] = {0x7F,0x41,0x41,0x22,0x1C};
    static const uint8_t E[5] = {0x7F,0x49,0x49,0x49,0x41};
    static const uint8_t F[5] = {0x7F,0x09,0x09,0x09,0x01};
    static const uint8_t G[5] = {0x3E,0x41,0x49,0x49,0x7A};
    static const uint8_t I[5] = {0x00,0x41,0x7F,0x41,0x00};
    static const uint8_t K[5] = {0x7F,0x08,0x14,0x22,0x41};
    static const uint8_t L[5] = {0x7F,0x40,0x40,0x40,0x40};
    static const uint8_t M[5] = {0x7F,0x02,0x0C,0x02,0x7F};
    static const uint8_t N[5] = {0x7F,0x02,0x04,0x08,0x7F};
    static const uint8_t O[5] = {0x3E,0x41,0x41,0x41,0x3E};
    static const uint8_t P[5] = {0x7F,0x09,0x09,0x09,0x06};
    static const uint8_t R[5] = {0x7F,0x09,0x19,0x29,0x46};
    static const uint8_t S[5] = {0x46,0x49,0x49,0x49,0x31};
    static const uint8_t T[5] = {0x01,0x01,0x7F,0x01,0x01};
    static const uint8_t U[5] = {0x3F,0x40,0x40,0x40,0x3F};
    static const uint8_t W[5] = {0x7F,0x20,0x18,0x20,0x7F};
    static const uint8_t Y[5] = {0x07,0x08,0x70,0x08,0x07};

    switch (c) {
        case '-': return DASH;
        case '.': return DOT;
        case '0': return NUM0;
        case '1': return NUM1;
        case '2': return NUM2;
        case '3': return NUM3;
        case '4': return NUM4;
        case '5': return NUM5;
        case '6': return NUM6;
        case '7': return NUM7;
        case '8': return NUM8;
        case '9': return NUM9;
        case 'A': return A;
        case 'C': return C;
        case 'D': return D;
        case 'E': return E;
        case 'F': return F;
        case 'G': return G;
        case 'I': return I;
        case 'K': return K;
        case 'L': return L;
        case 'M': return M;
        case 'N': return N;
        case 'O': return O;
        case 'P': return P;
        case 'R': return R;
        case 'S': return S;
        case 'T': return T;
        case 'U': return U;
        case 'W': return W;
        case 'Y': return Y;
        default: return SPACE;
    }
}

/*
   Draw one 5x7 character at selected scale.
*/
static void draw_char5x7(oled_t *d, char c, int x, int y, int scale)
{
    const uint8_t *glyph = font5x7(c);

    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
            if (glyph[col] & (1 << row)) {
                fill_rect(
                    d,
                    x + col * scale,
                    y + row * scale,
                    scale,
                    scale
                );
            }
        }
    }
}

/*
   Draw text centered on one OLED.
*/
static void draw_text_center(oled_t *d, const char *text)
{
    oled_clear(d);

    int w = logical_width(d);
    int h = logical_height(d);
    int len = strlen(text);

    /*
       Auto-scale text so it fits screen.
    */
    int scale_x = w / (len * 6);
    int scale_y = h / 8;
    int scale = scale_x < scale_y ? scale_x : scale_y;

    if (scale < 1) scale = 1;
    if (scale > 5) scale = 5;

    int text_w = len * 6 * scale - scale;
    int text_h = 7 * scale;

    int x = (w - text_w) / 2;
    int y = (h - text_h) / 2;

    if (x < 0) x = 0;
    if (y < 0) y = 0;

    for (int i = 0; i < len; i++) {
        draw_char5x7(d, text[i], x + i * 6 * scale, y, scale);
    }

    oled_display(d);
}

/*
   Draw small text at an exact position.
*/
static void draw_text_small(oled_t *d, const char *text, int x, int y)
{
    for (int i = 0; text[i] != '\0'; i++) {
        draw_char5x7(d, text[i], x + i * 5, y, 1);
    }
}

/*
   Draw small text mirrored horizontally.

   This is used for the setup IP line on the physically rotated first OLED.
*/
static void draw_text_small_mirrored(oled_t *d, const char *text, int x, int y)
{
    int len = strlen(text);

    for (int i = 0; i < len; i++) {
        draw_char5x7(d, text[i], x + (len - 1 - i) * 5, y, 1);
    }
}

/*
   Show same status message on all 5 OLEDs.
*/
static void show_status_all(const char *msg)
{
    ESP_LOGI(TAG, "STATUS: %s", msg);

    for (int i = 0; i < 5; i++) {
        draw_text_center(&displays[i], msg);
    }
}

/*
   Show all hotspot credentials and setup IP on the first OLED only.
*/
static void show_setup_details(void)
{
    for (int i = 0; i < 5; i++) {
        oled_clear(&displays[i]);
        oled_display(&displays[i]);
    }

    draw_text_small(&displays[0], "SSID", 2, 12);
    draw_text_small(&displays[0], SETUP_AP_SSID, 2, 22);
    draw_text_small(&displays[0], "PASS", 2, 42);
    draw_text_small(&displays[0], SETUP_AP_PASS, 2, 52);
    draw_text_small(&displays[0], "IP", 2, 72);
    draw_text_small_mirrored(&displays[0], "192.168.4.1", 2, 82);

    oled_display(&displays[0]);
}

/*
   Draw one digit using seven-segment style.

   Segment map:
       000
      5   1
      5   1
       666
      4   2
      4   2
       333
*/
static esp_err_t draw_digit(oled_t *d, int digit)
{
    oled_clear(d);

    int w = logical_width(d);
    int h = logical_height(d);

    int margin_x = 2;
    int margin_y = 5;

    /*
       Segment thickness.
    */
    int t = h / 9;
    if (t < 5) t = 5;

    int x_left = margin_x;
    int x_right = w - margin_x - t;

    int y_top = margin_y;
    int y_mid = h / 2 - t / 2;
    int y_bottom = h - margin_y - t;

    int hseg_x = margin_x + t;
    int hseg_w = w - 2 * margin_x - 2 * t;

    int upper_v_y = y_top + t - 1;
    int upper_v_h = y_mid - upper_v_y + 2;

    int lower_v_y = y_mid + t - 1;
    int lower_v_h = y_bottom - lower_v_y + 2;

    /*
       Seven segment states.
    */
    bool seg[7] = {0};

    switch (digit) {
        case 0:
            seg[0] = seg[1] = seg[2] = seg[3] = seg[4] = seg[5] = true;
            break;

        case 1:
            seg[1] = seg[2] = true;
            break;

        case 2:
            seg[0] = seg[1] = seg[6] = seg[4] = seg[3] = true;
            break;

        case 3:
            seg[0] = seg[1] = seg[6] = seg[2] = seg[3] = true;
            break;

        case 4:
            seg[5] = seg[6] = seg[1] = seg[2] = true;
            break;

        case 5:
            seg[0] = seg[5] = seg[6] = seg[2] = seg[3] = true;
            break;

        case 6:
            seg[0] = seg[5] = seg[6] = seg[4] = seg[2] = seg[3] = true;
            break;

        case 7:
            seg[0] = seg[1] = seg[2] = true;
            break;

        case 8:
            for (int i = 0; i < 7; i++) {
                seg[i] = true;
            }
            break;

        case 9:
            seg[0] = seg[1] = seg[2] = seg[3] = seg[5] = seg[6] = true;
            break;
    }

    /*
       Draw active segments.
    */
    if (seg[0]) draw_h_segment(d, hseg_x, y_top, hseg_w, t);

    if (seg[1]) draw_v_segment_right(d, x_right, upper_v_y, t, upper_v_h);
    if (seg[2]) draw_v_segment_right(d, x_right, lower_v_y, t, lower_v_h);

    if (seg[3]) draw_h_segment(d, hseg_x, y_bottom, hseg_w, t);

    if (seg[4]) draw_v_segment_left(d, x_left, lower_v_y, t, lower_v_h);
    if (seg[5]) draw_v_segment_left(d, x_left, upper_v_y, t, upper_v_h);

    if (seg[6]) draw_h_segment(d, hseg_x, y_mid, hseg_w, t);

    return oled_display(d);
}

/*
   Draw a tiny no-internet mark in the corner of the colon display.
*/
static void draw_no_internet_icon(oled_t *d)
{
    int w = logical_width(d);
    int x = w - 12;
    int y = 4;

    if (x < 1) {
        x = 1;
    }

    fill_rect(d, x + 1, y, 8, 2);
    fill_rect(d, x + 3, y + 4, 4, 2);
    fill_rect(d, x + 5, y + 8, 2, 2);

    for (int i = 0; i < 10; i++) {
        fill_rect(d, x + i, y + i, 2, 2);
    }
}

/*
   Draw blinking colon on middle display.

   Uses small horizontal beveled blocks instead of round dots,
   so it matches the seven-segment digit style.
*/
static esp_err_t draw_colon(oled_t *d, bool on, bool no_internet)
{
    oled_clear(d);

    if (on) {
        int w = logical_width(d);
        int h = logical_height(d);

        int block_w = w / 2;
        int block_h = h / 6;

        if (block_w < 14) block_w = 14;
        if (block_h < 6) block_h = 6;

        if (block_w > 28) block_w = 28;
        if (block_h > 10) block_h = 10;

        int x = (w - block_w) / 2;

        int top_y = h / 3 - block_h / 2;
        int bot_y = (2 * h) / 3 - block_h / 2;

        draw_h_segment(d, x, top_y, block_w, block_h);
        draw_h_segment(d, x, bot_y, block_w, block_h);
    }

    if (no_internet) {
        draw_no_internet_icon(d);
    }

    return oled_display(d);
}

/*
   Initialize ESP32 I2C peripheral.
*/
static void i2c_init_main(void)
{
    ESP_LOGI(TAG, "Init I2C SDA=%d SCL=%d", I2C_SDA_GPIO, I2C_SCL_GPIO);

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };

    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &conf));

    ESP_ERROR_CHECK(i2c_driver_install(
        I2C_PORT,
        conf.mode,
        0,
        0,
        0
    ));

    ESP_LOGI(TAG, "I2C OK");
}

/*
   Scan main I2C bus.

   This should usually find PCA9548A at 0x70.
*/
static void i2c_scan(void)
{
    ESP_LOGI(TAG, "Scan main I2C");

    for (uint8_t addr = 1; addr < 127; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();

        i2c_master_start(cmd);

        i2c_master_write_byte(
            cmd,
            (addr << 1) | I2C_MASTER_WRITE,
            true
        );

        i2c_master_stop(cmd);

        esp_err_t err = i2c_master_cmd_begin(
            I2C_PORT,
            cmd,
            pdMS_TO_TICKS(50)
        );

        i2c_cmd_link_delete(cmd);

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Found main bus device 0x%02X", addr);
        }
    }
}

/*
   Scan each PCA9548A channel.

   This helps confirm every OLED is found.
*/
static void scan_pca_channels(void)
{
    ESP_LOGI(TAG, "Scan PCA9548A channels");

    for (uint8_t ch = 0; ch < 5; ch++) {
        if (pca_select_channel(ch) != ESP_OK) {
            ESP_LOGE(TAG, "Cannot select channel %d", ch);
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(20));

        for (uint8_t addr = 1; addr < 127; addr++) {
            if (addr == PCA9548A_ADDR) continue;

            i2c_cmd_handle_t cmd = i2c_cmd_link_create();

            i2c_master_start(cmd);

            i2c_master_write_byte(
                cmd,
                (addr << 1) | I2C_MASTER_WRITE,
                true
            );

            i2c_master_stop(cmd);

            esp_err_t err = i2c_master_cmd_begin(
                I2C_PORT,
                cmd,
                pdMS_TO_TICKS(50)
            );

            i2c_cmd_link_delete(cmd);

            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Channel %d found device 0x%02X", ch, addr);
            }
        }
    }
}

/*
   Load saved Wi-Fi credentials from NVS.
*/
static bool load_wifi_credentials(wifi_credentials_t *creds)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs);

    if (err != ESP_OK) {
        return false;
    }

    size_t ssid_len = sizeof(creds->ssid);
    size_t pass_len = sizeof(creds->password);

    err = nvs_get_str(nvs, WIFI_NVS_SSID_KEY, creds->ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, WIFI_NVS_PASS_KEY, creds->password, &pass_len);
    }

    nvs_close(nvs);

    if (err != ESP_OK || creds->ssid[0] == '\0') {
        return false;
    }

    return true;
}

/*
   Save Wi-Fi credentials and commit them to flash.
*/
static esp_err_t save_wifi_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);

    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, WIFI_NVS_SSID_KEY, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, WIFI_NVS_PASS_KEY, password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}

/*
   Decode URL form values from the setup page.
*/
static void url_decode(char *dst, const char *src, size_t dst_len)
{
    size_t di = 0;

    while (*src && di + 1 < dst_len) {
        if (*src == '+') {
            dst[di++] = ' ';
            src++;
        } else if (*src == '%' &&
                   src[1] != '\0' &&
                   src[2] != '\0') {
            char hex[3] = { src[1], src[2], '\0' };
            dst[di++] = (char) strtol(hex, NULL, 16);
            src += 3;
        } else {
            dst[di++] = *src++;
        }
    }

    dst[di] = '\0';
}

/*
   Pull one field out of an application/x-www-form-urlencoded body.
*/
static bool form_get_value(
    const char *body,
    const char *key,
    char *value,
    size_t value_len)
{
    size_t key_len = strlen(key);
    const char *p = body;

    while (p && *p) {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            p += key_len + 1;

            const char *end = strchr(p, '&');
            size_t encoded_len = end ? (size_t)(end - p) : strlen(p);

            char encoded[96];
            if (encoded_len >= sizeof(encoded)) {
                encoded_len = sizeof(encoded) - 1;
            }

            memcpy(encoded, p, encoded_len);
            encoded[encoded_len] = '\0';

            url_decode(value, encoded, value_len);
            return true;
        }

        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }

    return false;
}

static esp_err_t setup_page_get_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP32-C6 Clock Setup</title>"
        "<style>"
        "body{font-family:system-ui,Arial;margin:2rem;max-width:28rem}"
        "label,input,button{display:block;width:100%;font-size:1rem}"
        "input{box-sizing:border-box;margin:.35rem 0 1rem;padding:.7rem}"
        "button{padding:.8rem;background:#111;color:white;border:0}"
        "</style></head><body>"
        "<h1>Clock Wi-Fi Setup</h1>"
        "<form method='post' action='/save'>"
        "<label>Wi-Fi name</label><input name='ssid' maxlength='32' required>"
        "<label>Password</label><input name='pass' maxlength='64' type='password'>"
        "<button type='submit'>Save and restart</button>"
        "</form></body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t setup_save_post_handler(httpd_req_t *req)
{
    char body[192];
    int total = 0;

    while (total < req->content_len && total < (int)sizeof(body) - 1) {
        int ret = httpd_req_recv(
            req,
            body + total,
            sizeof(body) - 1 - total
        );

        if (ret <= 0) {
            return ESP_FAIL;
        }

        total += ret;
    }

    body[total] = '\0';

    char ssid[33] = {0};
    char password[65] = {0};

    bool has_ssid = form_get_value(body, "ssid", ssid, sizeof(ssid));
    form_get_value(body, "pass", password, sizeof(password));

    if (!has_ssid || ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing Wi-Fi name");
        return ESP_OK;
    }

    esp_err_t err = save_wifi_credentials(ssid, password);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(
        req,
        "<!doctype html><html><body>"
        "<h1>Saved</h1><p>The clock is restarting now.</p>"
        "</body></html>"
    );

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;
static bool wifi_core_ready = false;
static bool wifi_driver_ready = false;
static bool wifi_handlers_registered = false;
static bool wifi_started = false;

/*
   ESP-IDF Wi-Fi and event-loop setup shared by STA and setup AP mode.
*/
static void wifi_core_init(void)
{
    if (!wifi_core_ready) {
        ESP_ERROR_CHECK(esp_netif_init());

        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(err);
        }

        wifi_core_ready = true;
    }

    if (!wifi_driver_ready) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        wifi_driver_ready = true;
    }
}

static void wifi_stop_if_started(void)
{
    if (wifi_started) {
        esp_err_t err = esp_wifi_stop();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_ERROR_CHECK(err);
        }

        wifi_started = false;
    }
}

/*
   Start setup hotspot and web page. This function does not return.
*/
static void start_setup_portal(void)
{
    ESP_LOGW(TAG, "Starting setup hotspot");
    ESP_LOGW(TAG, "Connect to Wi-Fi SSID: %s", SETUP_AP_SSID);
    ESP_LOGW(TAG, "Hotspot password: %s", SETUP_AP_PASS);
    ESP_LOGW(TAG, "Open this address in a browser: http://192.168.4.1");

    show_setup_details();

    wifi_core_init();
    wifi_stop_if_started();

    if (ap_netif == NULL) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_config_t ap_config = {
        .ap = {
            .ssid = SETUP_AP_SSID,
            .ssid_len = strlen(SETUP_AP_SSID),
            .password = SETUP_AP_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };

    if (strlen(SETUP_AP_PASS) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_started = true;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = setup_page_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t save_uri = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = setup_save_post_handler,
        .user_ctx = NULL,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &save_uri));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/*
   Wi-Fi event handler.

   ESP-IDF calls this automatically when Wi-Fi events happen.
*/
static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {

        ESP_LOGI(TAG, "WiFi connect");
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {

        wifi_connected = false;
        wifi_retry_count++;

        ESP_LOGW(TAG, "WiFi reconnect");
        esp_wifi_connect();

    } else if (event_base == IP_EVENT &&
               event_id == IP_EVENT_STA_GOT_IP) {

        ESP_LOGI(TAG, "WiFi connected");
        wifi_connected = true;
        wifi_retry_count = 0;

        xEventGroupSetBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT
        );
    }
}

/*
   Start Wi-Fi and wait up to 20 seconds.

   Returns:
   true  = connected
   false = timeout / no Wi-Fi
*/
static bool wifi_init_main(const wifi_credentials_t *creds)
{
    ESP_LOGI(TAG, "Init WiFi");

    wifi_event_group = xEventGroupCreate();

    wifi_core_init();

    if (sta_netif == NULL) {
        sta_netif = esp_netif_create_default_wifi_sta();
    }

    if (!wifi_handlers_registered) {
        ESP_ERROR_CHECK(
            esp_event_handler_instance_register(
                WIFI_EVENT,
                ESP_EVENT_ANY_ID,
                &wifi_event_handler,
                NULL,
                NULL
            )
        );

        ESP_ERROR_CHECK(
            esp_event_handler_instance_register(
                IP_EVENT,
                IP_EVENT_STA_GOT_IP,
                &wifi_event_handler,
                NULL,
                NULL
            )
        );

        wifi_handlers_registered = true;
    }

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "",
            .password = "",
        },
    };

    strncpy((char *)wifi_config.sta.ssid, creds->ssid, sizeof(wifi_config.sta.ssid));
    strncpy(
        (char *)wifi_config.sta.password,
        creds->password,
        sizeof(wifi_config.sta.password)
    );

    wifi_connected = false;
    wifi_retry_count = 0;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wifi_config
        )
    );

    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_started = true;

    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT,
        false,
        true,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS)
    );

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi ready");
        return true;
    }

    ESP_LOGE(TAG, "WiFi timeout");
    return false;
}

/*
   Sync time from internet using NTP.

   Timezone is set for Ireland / UK:
   GMT in winter, BST/IST in summer.

   Returns:
   true  = time synced
   false = NTP failed
*/
static bool time_sync_init(void)
{
    ESP_LOGI(TAG, "NTP sync");

    setenv(
        "TZ",
        "GMT0BST,M3.5.0/1,M10.5.0/2",
        1
    );

    tzset();

    if (!sntp_started) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.google.com");
        esp_sntp_init();
        sntp_started = true;
    }

    time_t now = 0;
    struct tm timeinfo = {0};

    int retry = 0;

    while (timeinfo.tm_year < (2024 - 1900) && retry < 30) {
        ESP_LOGI(TAG, "Wait NTP %d", retry + 1);

        show_status_all("SYNC");

        vTaskDelay(pdMS_TO_TICKS(1000));

        time(&now);
        localtime_r(&now, &timeinfo);

        retry++;
    }

    if (timeinfo.tm_year >= (2024 - 1900)) {
        ESP_LOGI(TAG, "NTP OK");
        return true;
    }

    ESP_LOGE(TAG, "NTP failed; keeping WiFi credentials and retrying in background");
    return false;
}

/*
   Main program starts here.
*/
void app_main(void)
{
    ESP_LOGI(TAG, "CLOCK START");

    /*
       NVS is required by Wi-Fi.
    */
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    /*
       Start I2C and scan devices.
    */
    i2c_init_main();

    i2c_scan();
    scan_pca_channels();

    /*
       Initialize all OLEDs.
    */
    for (int i = 0; i < 5; i++) {
        esp_err_t err = oled_init(&displays[i]);

        if (err == ESP_OK) {
            oled_clear(&displays[i]);
            oled_display(&displays[i]);
        } else {
            ESP_LOGE(TAG, "OLED FAIL %d", i);
        }
    }

    /*
       Show connection status.
    */
    show_status_all("CONNECT");

    wifi_credentials_t creds = {0};

    if (!load_wifi_credentials(&creds)) {
        ESP_LOGW(TAG, "No saved WiFi credentials");
        start_setup_portal();
    }

    bool wifi_ok = wifi_init_main(&creds);

    if (!wifi_ok) {
        ESP_LOGW(TAG, "WiFi failed; entering setup mode");
        start_setup_portal();
    }

    /*
       Sync time from internet.
    */
    bool time_ok = time_sync_init();

    if (!time_ok) {
        ESP_LOGW(TAG, "NTP failed; staying online and waiting for time");
        show_status_all("NO NET");
    }

    /*
       Store previous minute so digit OLEDs update only when minute changes.
       Colon still updates every second.
    */
    int last_minute = -1;
    int last_no_net_second = -1;

    while (1) {
        time_t now;
        struct tm t;

        /*
           Get current local time.
        */
        time(&now);
        localtime_r(&now, &t);

        bool time_valid = t.tm_year >= (2024 - 1900);

        ESP_LOGI(
            TAG,
            "TIME %02d:%02d:%02d wifi=%s valid=%s",
            t.tm_hour,
            t.tm_min,
            t.tm_sec,
            wifi_connected ? "yes" : "no",
            time_valid ? "yes" : "no"
        );

        if (!time_valid) {
            if (t.tm_sec != last_no_net_second &&
                (last_no_net_second < 0 || t.tm_sec % 10 == 0)) {
                last_no_net_second = t.tm_sec;
                show_status_all("NO NET");
            }

            draw_colon(&displays[2], true, true);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /*
           Update digit screens only when minute changes.
        */
        if (t.tm_min != last_minute) {
            last_minute = t.tm_min;

            int h = t.tm_hour;
            int m = t.tm_min;

            /*
               Your physical layout needs hour digits in this order.
            */
            draw_digit(&displays[0], h % 10);
            draw_digit(&displays[1], h / 10);

            draw_digit(&displays[3], m / 10);
            draw_digit(&displays[4], m % 10);
        }

        /*
           Blink colon every second.
           Even second = ON
           Odd second  = OFF
        */
        bool colon_on = (t.tm_sec % 2 == 0);

        draw_colon(&displays[2], colon_on, !wifi_connected);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
