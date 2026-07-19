/* Management console on CDC-ACM 0 of the composite USB device, plus the NVS
 * credential store. Runtime provisioning: credentials set here take effect
 * immediately and survive reboots, so the compile-time defaults are only a
 * fallback for a never-provisioned device.
 *
 * Command handling runs in the TinyUSB task context (CDC RX callback); commands
 * are short and NVS writes take a few ms, which the bridge tolerates. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "tinyusb_cdc_acm.h"

#include "bridge.h"

static const char *TAG = "console";

#define NVS_NAMESPACE "wificfg"
#define SSID_MAX 32
#define PASS_MAX 64

/* Working copy of the credentials: edited by `set`, applied immediately,
 * persisted by `save`. */
static char s_ssid[SSID_MAX + 1];
static char s_pass[PASS_MAX + 1];

static char s_line[128];
static size_t s_line_len;

static void con_puts(const char *s)
{
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)s, strlen(s));
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
}

static void con_printf(const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    con_puts(buf);
}

void creds_load(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    ssid[0] = pass[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t n = ssid_sz;
        if (nvs_get_str(h, "ssid", ssid, &n) != ESP_OK) {
            ssid[0] = '\0';
        }
        n = pass_sz;
        if (nvs_get_str(h, "pass", pass, &n) != ESP_OK) {
            pass[0] = '\0';
        }
        nvs_close(h);
    }
    if (ssid[0] == '\0') { /* never provisioned: compile-time fallback */
        strlcpy(ssid, CONFIG_ESP_WIFI_SSID, ssid_sz);
        strlcpy(pass, CONFIG_ESP_WIFI_PASSWORD, pass_sz);
    }
}

static esp_err_t creds_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "ssid", s_ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "pass", s_pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void creds_clear_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void show_state(void)
{
    bridge_stats_t st;
    bridge_get_stats(&st);
    uint8_t mac[6];
    bridge_get_mac(mac);

    con_printf("    ssid:      %s\r\n", s_ssid[0] ? s_ssid : "(unset)");
    con_printf("    pass:      %s\r\n", s_pass[0] ? "set" : "unset (open)");
    con_printf("    status:    %s\r\n",
               bridge_wifi_connected() ? "associated"
               : (s_ssid[0] ? "associating" : "unprovisioned"));
    con_printf("    mac:       %02x:%02x:%02x:%02x:%02x:%02x (host adopts it)\r\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    uint8_t ip4[4];
    if (bridge_host_ipv4(ip4)) {
        con_printf("    host IPv4: %u.%u.%u.%u\r\n", ip4[0], ip4[1], ip4[2], ip4[3]);
    } else {
        con_puts("    host IPv4: (not seen yet)\r\n");
    }
    uint8_t ip6[16];
    if (bridge_host_ipv6(ip6)) {
        con_printf("    host IPv6: %x:%x:%x:%x:%x:%x:%x:%x\r\n",
                   (ip6[0] << 8) | ip6[1], (ip6[2] << 8) | ip6[3],
                   (ip6[4] << 8) | ip6[5], (ip6[6] << 8) | ip6[7],
                   (ip6[8] << 8) | ip6[9], (ip6[10] << 8) | ip6[11],
                   (ip6[12] << 8) | ip6[13], (ip6[14] << 8) | ip6[15]);
    } else {
        con_puts("    host IPv6: (not seen yet)\r\n");
    }
    con_printf("    stats:     ->wifi=%lu ->host=%lu txdrop=%lu refl=%lu\r\n",
               (unsigned long)st.host_to_wifi, (unsigned long)st.wifi_to_host,
               (unsigned long)st.txdrop, (unsigned long)st.reflected);
}

static void prompt(void)
{
    con_puts("(set|show|save|clear) # ");
}

static void banner(void)
{
    con_puts("\r\n-- esp32-usb-eth --\r\n");
    show_state();
    prompt();
}

static void handle_line(char *line)
{
    /* strip leading spaces */
    while (*line == ' ') {
        line++;
    }
    if (line[0] == '\0') {
        return;
    }

    if (strcmp(line, "help") == 0) {
        con_puts("    set ssid <text>   set SSID and re-associate\r\n"
                 "    set pass <text>   set passphrase (blank for open) and re-associate\r\n"
                 "    show              show state\r\n"
                 "    save              persist credentials to NVS\r\n"
                 "    clear             erase saved credentials\r\n"
                 "    reboot            restart the device\r\n");
    } else if (strcmp(line, "show") == 0) {
        show_state();
    } else if (strncmp(line, "set ssid", 8) == 0 && (line[8] == ' ' || line[8] == '\0')) {
        strlcpy(s_ssid, line[8] ? line + 9 : "", sizeof(s_ssid));
        con_puts("[*] applying -- re-associating\r\n");
        wifi_apply_creds(s_ssid, s_pass);
    } else if (strncmp(line, "set pass", 8) == 0 && (line[8] == ' ' || line[8] == '\0')) {
        strlcpy(s_pass, line[8] ? line + 9 : "", sizeof(s_pass));
        con_puts("[*] applying -- re-associating\r\n");
        wifi_apply_creds(s_ssid, s_pass);
    } else if (strcmp(line, "save") == 0) {
        con_puts(creds_save() == ESP_OK ? "[*] saved to NVS\r\n" : "[!] save failed\r\n");
    } else if (strcmp(line, "clear") == 0) {
        creds_clear_nvs();
        con_puts("[*] saved credentials erased (running config unchanged)\r\n");
    } else if (strcmp(line, "reboot") == 0) {
        con_puts("[*] rebooting\r\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else {
        con_puts("[!] unknown command; try help\r\n");
    }
}

static void cdc_rx_cb(int itf, cdcacm_event_t *event)
{
    uint8_t buf[64];
    size_t n = 0;
    while (tinyusb_cdcacm_read(itf, buf, sizeof(buf), &n) == ESP_OK && n > 0) {
        for (size_t i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\r' || c == '\n') {
                if (s_line_len == 0) {
                    continue;
                }
                con_puts("\r\n");
                s_line[s_line_len] = '\0';
                s_line_len = 0;
                handle_line(s_line);
                prompt();
            } else if (c == 0x7f || c == 0x08) { /* backspace */
                if (s_line_len > 0) {
                    s_line_len--;
                    con_puts("\b \b");
                }
            } else if (c >= 0x20 && c < 0x7f && s_line_len < sizeof(s_line) - 1) {
                s_line[s_line_len++] = c;
                tinyusb_cdcacm_write_queue_char(itf, c); /* echo */
                tinyusb_cdcacm_write_flush(itf, 0);
            }
        }
        if (n < sizeof(buf)) {
            break;
        }
    }
}

static void cdc_line_state_cb(int itf, cdcacm_event_t *event)
{
    /* terminal opened (DTR raised): greet it */
    if (event->line_state_changed_data.dtr) {
        banner();
    }
}

void console_init(void)
{
    /* seed the working copy with whatever the bridge booted with */
    creds_load(s_ssid, sizeof(s_ssid), s_pass, sizeof(s_pass));

    const tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = cdc_rx_cb,
        .callback_line_state_changed = cdc_line_state_cb,
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm_cfg));
    ESP_LOGI(TAG, "management console on CDC-ACM 0");
}
