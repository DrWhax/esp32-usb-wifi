/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Based on the esp-idf tusb_ncm example; extended into the esp32-usb-eth
 * transparent L2 bridge (see README.md and pico-usb-wifi/esp32_plan.md):
 * reflection filter, host address snooping, and a CDC-ACM management console
 * with runtime credentials in NVS (console.c).
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "esp_mac.h"

#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"

#include "bridge.h"

static const char *TAG = "USB_NCM";

static bool s_is_wifi_connected;
static uint8_t s_mac[6];      /* station MAC; the host's NCM interface adopts it */
static char s_ssid[33];       /* active credentials (console may replace them) */
static char s_pass[65];

/* Bridged-frame counters (32-bit aligned writes are atomic on Xtensa; the
 * console only reads them for display). */
static volatile uint32_t s_cnt_host_to_wifi;
static volatile uint32_t s_cnt_wifi_to_host;
static volatile uint32_t s_cnt_txdrop;
static volatile uint32_t s_cnt_reflected;
static volatile uint32_t s_cnt_poolfail;

static uint8_t s_last_disc_reason;    /* wifi_err_reason_t of the last disconnect */
static esp_timer_handle_t s_retry_timer; /* paced re-join, instead of a tight loop */

/* Host addresses, snooped passively from host -> Wi-Fi frames: the bridge
 * holds no IP, so this is the only way the console can report what address
 * the host obtained. */
static volatile bool s_host_ip4_valid;
static uint8_t s_host_ip4[4];
static volatile bool s_host_ip6_valid;
static uint8_t s_host_ip6[16];

/* Inspect a host -> Wi-Fi frame and record the source address it advertises.
 * Best-effort, untagged Ethernet only. */
static void snoop_host_addr(const uint8_t *f, uint16_t len)
{
    if (len < 14) {
        return;
    }
    uint16_t eth = (uint16_t)((f[12] << 8) | f[13]);
    if (eth == 0x0806 && len >= 32) { /* ARP: sender protocol (IPv4) address at 28 */
        if (f[28] | f[29] | f[30] | f[31]) {
            memcpy(s_host_ip4, f + 28, 4);
            s_host_ip4_valid = true;
        }
    } else if (eth == 0x0800 && len >= 30) { /* IPv4: source address at 26 */
        if (f[26] | f[27] | f[28] | f[29]) {
            memcpy(s_host_ip4, f + 26, 4);
            s_host_ip4_valid = true;
        }
    } else if (eth == 0x86dd && len >= 38) { /* IPv6: source address at 22 */
        if ((f[22] & 0xe0) == 0x20) { /* 2000::/3 global unicast only */
            memcpy(s_host_ip6, f + 22, 16);
            s_host_ip6_valid = true;
        }
    }
}

static esp_err_t usb_recv_callback(void *buffer, uint16_t len, void *ctx)
{
    snoop_host_addr(buffer, len);
    if (s_is_wifi_connected) {
        if (esp_wifi_internal_tx(ESP_IF_WIFI_STA, buffer, len) == ESP_OK) {
            s_cnt_host_to_wifi++;
        } else {
            s_cnt_poolfail++; /* driver out of TX buffers; the host retries */
        }
    } else {
        s_cnt_txdrop++; /* not associated; the host retries */
    }
    return ESP_OK;
}

static void wifi_pkt_free(void *eb, void *ctx)
{
    esp_wifi_internal_free_rx_buffer(eb);
}

static esp_err_t pkt_wifi2usb(void *buffer, uint16_t len, void *eb)
{
    /* Reflection filter: the AP floods the host's own broadcast/multicast back
     * to the station, whose MAC the host shares. A bridge must not echo a
     * station's frames back to it (prevents IPv6 DAD / IPv4 ACD false
     * positives and mDNS self-answers). */
    if (len >= 12 && memcmp((const uint8_t *)buffer + 6, s_mac, 6) == 0) {
        s_cnt_reflected++;
        esp_wifi_internal_free_rx_buffer(eb);
        return ESP_OK;
    }
    if (tinyusb_net_send_sync(buffer, len, eb, portMAX_DELAY) != ESP_OK) {
        esp_wifi_internal_free_rx_buffer(eb);
    } else {
        s_cnt_wifi_to_host++;
    }
    return ESP_OK;
}

static void retry_timer_cb(void *arg)
{
    if (!s_is_wifi_connected && s_ssid[0]) {
        esp_wifi_connect();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = event_data;
        s_last_disc_reason = d->reason;
        ESP_LOGI(TAG, "WiFi STA disconnected (reason %d)", d->reason);
        if (s_is_wifi_connected) {
            console_debug_printf("Wi-Fi link down (reason %d)", d->reason);
        }
        s_is_wifi_connected = false;
        esp_wifi_internal_reg_rxcb(ESP_IF_WIFI_STA, NULL);
        if (s_ssid[0]) { /* paced re-join, unless unprovisioned */
            esp_timer_start_once(s_retry_timer, 5 * 1000 * 1000);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "WiFi STA connected");
        esp_wifi_internal_reg_rxcb(ESP_IF_WIFI_STA, pkt_wifi2usb);
        s_is_wifi_connected = true;
        s_last_disc_reason = 0;
        console_debug_printf("associated to %s", s_ssid);
    }
}

static void wifi_set_config_from_creds(void)
{
    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid, s_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, s_pass, sizeof(wifi_config.sta.password));
    /* WPA2/WPA3 transition, like the pico firmware: join either kind of AP */
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
}

/* --- interface for the console (bridge.h) ------------------------------- */

void bridge_get_stats(bridge_stats_t *s)
{
    s->host_to_wifi = s_cnt_host_to_wifi;
    s->wifi_to_host = s_cnt_wifi_to_host;
    s->txdrop = s_cnt_txdrop;
    s->reflected = s_cnt_reflected;
    s->poolfail = s_cnt_poolfail;
}

const char *bridge_link_status(void)
{
    if (s_is_wifi_connected) {
        return "up";
    }
    switch (s_last_disc_reason) {
        case WIFI_REASON_NO_AP_FOUND:
            return "nonet";
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "badauth";
        default:
            return "join";
    }
}

void bridge_get_mac(uint8_t mac[6])
{
    memcpy(mac, s_mac, 6);
}

bool bridge_wifi_connected(void)
{
    return s_is_wifi_connected;
}

bool bridge_host_ipv4(uint8_t ip[4])
{
    if (!s_host_ip4_valid) {
        return false;
    }
    memcpy(ip, s_host_ip4, 4);
    return true;
}

bool bridge_host_ipv6(uint8_t ip[16])
{
    if (!s_host_ip6_valid) {
        return false;
    }
    memcpy(ip, s_host_ip6, 16);
    return true;
}

void wifi_apply_creds(const char *ssid, const char *pass)
{
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, pass, sizeof(s_pass));
    esp_timer_stop(s_retry_timer); /* a pending retry would race the new config */
    esp_wifi_disconnect();
    wifi_set_config_from_creds();
    if (s_ssid[0]) {
        /* Join now; if this races the in-flight disconnect and fails, the
         * disconnect handler's retry timer re-joins within 5 s. */
        esp_wifi_connect();
    }
}

/* ------------------------------------------------------------------------ */

static esp_err_t start_wifi(void)
{
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_cfg), TAG, "Failed to initialize WiFi library");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL),
                        TAG, "Failed to register handler for wifi events");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Failed to set WiFi station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Failed to start WiFi library");

    wifi_set_config_from_creds();
    if (s_ssid[0] == '\0') {
        ESP_LOGW(TAG, "no Wi-Fi SSID configured; provision over the CDC-ACM console");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "associating to '%s'", s_ssid);
    return esp_wifi_connect();
}

void app_main(void)
{
    /* Initialize NVS — PHY calibration data and the credential store */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* NVS credentials if provisioned, else compile-time defaults */
    creds_load(s_ssid, sizeof(s_ssid), s_pass, sizeof(s_pass));

    ESP_LOGI(TAG, "USB NCM device initialization");
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_GOTO_ON_ERROR(tinyusb_driver_install(&tusb_cfg), err, TAG, "Failed to install TinyUSB driver");

    tinyusb_net_config_t net_config = {
        .on_recv_callback = usb_recv_callback,
        .free_tx_buffer = wifi_pkt_free,
    };
    esp_read_mac(net_config.mac_addr, ESP_MAC_WIFI_STA);
    memcpy(s_mac, net_config.mac_addr, 6);
    ESP_LOGI(TAG, "Network interface HW address: %02x:%02x:%02x:%02x:%02x:%02x",
             s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
    ESP_GOTO_ON_ERROR(tinyusb_net_init(&net_config), err, TAG, "Failed to initialize TinyUSB NCM device class");

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    const esp_timer_create_args_t targs = {
        .callback = retry_timer_cb,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));

    console_init(); /* needs the event loop (scan-done handler) */

    ESP_LOGI(TAG, "WiFi initialization");
    ESP_GOTO_ON_ERROR(start_wifi(), err, TAG, "Failed to init and start WiFi");

    ESP_LOGI(TAG, "USB NCM and WiFi initialized and started");
    return;

err:
    ESP_LOGE(TAG, "USB-WiFi bridge failed to start!");
}
