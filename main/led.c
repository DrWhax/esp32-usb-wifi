/* Status LED on the devkit's WS2812 (pico-usb-wifi LED-state parity):
 *   solid            associated - the normal running state
 *   slow blink 1 Hz  Wi-Fi configured, associating
 *   fast blink 5 Hz  no Wi-Fi configured - provision over the console
 *   off              USB not ready
 *
 * The data pin varies between devkit revisions (48 on ESP32-S3-DevKitC-1
 * v1.0, 38 on v1.1) - set it with `idf.py menuconfig` -> Example
 * Configuration if the LED stays dark. */

#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "nvs.h"
#include "tusb.h"

#include "bridge.h"

#define LED_BRIGHTNESS 24 /* out of 255; the WS2812 is blinding at full scale */

static led_strip_handle_t s_strip;
static esp_timer_handle_t s_timer;
static int s_gpio = CONFIG_BRIDGE_LED_GPIO;
static bool s_lit; /* last written state, so we only touch the strip on change */
static bool s_have_ssid;

/* console.c updates this so the LED needn't reach into the config store */
void led_set_provisioned(bool have_ssid)
{
    s_have_ssid = have_ssid;
}

static void led_write(bool on)
{
    if (s_strip == NULL || on == s_lit) {
        return;
    }
    s_lit = on;
    if (on) {
        led_strip_set_pixel(s_strip, 0, 0, LED_BRIGHTNESS, 0); /* green */
        led_strip_refresh(s_strip);
    } else {
        led_strip_clear(s_strip);
    }
}

static void led_tick(void *arg)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    bool on;
    if (!tud_mounted()) {
        on = false;
    } else if (bridge_wifi_connected()) {
        on = true;
    } else if (!s_have_ssid) {
        on = (now % 200u) < 100u; /* 5 Hz: unprovisioned */
    } else {
        on = (now % 1000u) < 500u; /* 1 Hz: associating */
    }
    led_write(on);
}

/* (Re)create the strip on `gpio`. Called at boot and from the console's
 * `set led <gpio>`, so a wrong compile-time pin is fixable at runtime. The
 * tick timer is paused around the swap so it never touches a dying handle. */
bool led_set_gpio(int gpio)
{
    if (s_timer) {
        esp_timer_stop(s_timer);
    }
    if (s_strip) {
        led_strip_clear(s_strip);
        led_strip_del(s_strip);
        s_strip = NULL;
    }
    s_gpio = gpio;
    s_lit = false;

    const led_strip_config_t strip_cfg = {
        .strip_gpio_num = gpio,
        .max_leds = 1,
    };
    const led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    bool ok = (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip) == ESP_OK);
    if (!ok) {
        ESP_LOGW("led", "no WS2812 on GPIO%d; LED disabled", gpio);
        s_strip = NULL;
    } else {
        led_strip_clear(s_strip);
    }
    if (s_timer) {
        esp_timer_start_periodic(s_timer, 50 * 1000);
    }
    return ok;
}

void led_init(void)
{
    /* a console-set pin overrides the compile-time default */
    nvs_handle_t h;
    if (nvs_open("wificfg", NVS_READONLY, &h) == ESP_OK) {
        uint8_t gpio;
        if (nvs_get_u8(h, "ledgpio", &gpio) == ESP_OK) {
            s_gpio = gpio;
        }
        nvs_close(h);
    }

    const esp_timer_create_args_t targs = {
        .callback = led_tick,
        .name = "led",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_timer));
    led_set_gpio(s_gpio); /* starts the 20 Hz tick, like the pico */
}
