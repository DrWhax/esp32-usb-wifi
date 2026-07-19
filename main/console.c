/* Management console on CDC-ACM 0 of the composite USB device, plus the NVS
 * configuration store: up to 8 credential profiles, the active-profile index,
 * and the debug flag. Runtime provisioning: changes apply immediately and
 * `save` persists them, so the compile-time defaults are only a fallback for a
 * never-provisioned device.
 *
 * Command handling runs in the TinyUSB task context (CDC RX callback), so
 * nothing here may block: the scan is asynchronous (results are printed from
 * the WIFI_EVENT_SCAN_DONE handler) and the debug stats stream runs on an
 * esp_timer. Commands are short and NVS writes take a few ms, which the
 * bridge tolerates. */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "tinyusb_cdc_acm.h"

#include "bridge.h"

static const char *TAG = "console";

#define NVS_NAMESPACE "wificfg"
#define CFG_MAGIC_V1 0x45555731u /* "EUW1": no country field */
#define CFG_MAGIC 0x45555732u    /* "EUW2" */
#define MAX_PROFILES 8
#define MAX_SCAN 20

typedef struct {
    char ssid[33];
    char pass[65];
} profile_t;

typedef struct {
    uint32_t magic;
    uint8_t active; /* index into p[] */
    uint8_t debug;
    profile_t p[MAX_PROFILES];
} cfg_blob_v1_t;

typedef struct {
    uint32_t magic;
    uint8_t active; /* index into p[] */
    uint8_t debug;
    char country[3]; /* "" = driver default; "01" = world-safe */
    profile_t p[MAX_PROFILES];
} cfg_blob_t;

static cfg_blob_t s_cfg; /* working copy: edited by commands, persisted by `save` */

static char s_line[128];
static size_t s_line_len;

/* last scan results, for `join <n>` */
static wifi_ap_record_t s_scan[MAX_SCAN];
static int s_scan_count;
static bool s_scan_running;

static esp_timer_handle_t s_dbg_timer;

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

/* Diagnostics from the bridge (association edges) and the periodic stats
 * line; interleaved on the single console, prefixed and gated on the flag. */
void console_debug_printf(const char *fmt, ...)
{
    if (!s_cfg.debug) {
        return;
    }
    char buf[192] = "dbg: ";
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + 5, sizeof(buf) - 7, fmt, ap);
    va_end(ap);
    strlcat(buf, "\r\n", sizeof(buf));
    con_puts(buf);
}

/* --- config store ------------------------------------------------------- */

static profile_t *active_profile(void)
{
    return &s_cfg.p[s_cfg.active];
}

static int profile_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_PROFILES; i++) {
        if (s_cfg.p[i].ssid[0]) {
            n++;
        }
    }
    return n;
}

static void cfg_load(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.magic = CFG_MAGIC;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        union {
            cfg_blob_t v2;
            cfg_blob_v1_t v1;
        } tmp;
        size_t n = sizeof(tmp);
        if (nvs_get_blob(h, "cfg", &tmp, &n) == ESP_OK) {
            if (n == sizeof(cfg_blob_t) && tmp.v2.magic == CFG_MAGIC) {
                s_cfg = tmp.v2;
            } else if (n == sizeof(cfg_blob_v1_t) && tmp.v1.magic == CFG_MAGIC_V1) {
                s_cfg.active = tmp.v1.active; /* v1 record: no country yet */
                s_cfg.debug = tmp.v1.debug;
                memcpy(s_cfg.p, tmp.v1.p, sizeof(s_cfg.p));
            }
            if (s_cfg.active >= MAX_PROFILES) {
                s_cfg.active = 0;
            }
            if (s_cfg.p[0].ssid[0] || profile_count()) {
                nvs_close(h);
                return;
            }
        }
        /* migration: the pre-profile firmware stored plain ssid/pass keys */
        n = sizeof(s_cfg.p[0].ssid);
        if (nvs_get_str(h, "ssid", s_cfg.p[0].ssid, &n) == ESP_OK) {
            n = sizeof(s_cfg.p[0].pass);
            if (nvs_get_str(h, "pass", s_cfg.p[0].pass, &n) != ESP_OK) {
                s_cfg.p[0].pass[0] = '\0';
            }
            nvs_close(h);
            return;
        }
        nvs_close(h);
    }
    /* never provisioned: compile-time fallback */
    strlcpy(s_cfg.p[0].ssid, CONFIG_ESP_WIFI_SSID, sizeof(s_cfg.p[0].ssid));
    strlcpy(s_cfg.p[0].pass, CONFIG_ESP_WIFI_PASSWORD, sizeof(s_cfg.p[0].pass));
}

static esp_err_t cfg_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, "cfg", &s_cfg, sizeof(s_cfg));
    if (err == ESP_OK) {
        nvs_erase_key(h, "ssid"); /* drop migrated legacy keys, if any */
        nvs_erase_key(h, "pass");
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

void creds_load(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    cfg_load();
    strlcpy(ssid, active_profile()->ssid, ssid_sz);
    strlcpy(pass, active_profile()->pass, pass_sz);
    led_set_provisioned(active_profile()->ssid[0] != '\0');
}

const char *cfg_country(void)
{
    return s_cfg.country;
}

/* --- debug stats stream ------------------------------------------------- */

static void dbg_timer_cb(void *arg)
{
    bridge_stats_t st;
    bridge_get_stats(&st);
    uint8_t ip4[4];
    char ips[16] = "none";
    if (bridge_host_ipv4(ip4)) {
        snprintf(ips, sizeof(ips), "%u.%u.%u.%u", ip4[0], ip4[1], ip4[2], ip4[3]);
    }
    console_debug_printf("stats: ->wifi=%lu ->host=%lu txdrop=%lu rxdrop=%lu refl=%lu "
                         "poolfail=%lu link=%s freeram=%lu host=%s",
                         (unsigned long)st.host_to_wifi, (unsigned long)st.wifi_to_host,
                         (unsigned long)st.txdrop, (unsigned long)st.rxdrop,
                         (unsigned long)st.reflected, (unsigned long)st.poolfail,
                         bridge_link_status(), (unsigned long)esp_get_free_heap_size(), ips);
}

static void dbg_set(bool on)
{
    s_cfg.debug = on;
    if (on) {
        esp_timer_start_periodic(s_dbg_timer, 2 * 1000 * 1000);
    } else {
        esp_timer_stop(s_dbg_timer);
    }
}

/* --- scan --------------------------------------------------------------- */

static const char *auth_str(wifi_auth_mode_t m)
{
    return m == WIFI_AUTH_OPEN ? "open" : "";
}

static void scan_done_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (!s_scan_running) {
        return; /* a scan we did not start */
    }
    s_scan_running = false;
    uint16_t n = MAX_SCAN;
    if (esp_wifi_scan_get_ap_records(&n, s_scan) != ESP_OK) {
        con_puts("\r\n[!] scan failed\r\n");
        s_scan_count = 0;
        return;
    }
    s_scan_count = n;
    con_printf("\r\n    networks (%d):\r\n", s_scan_count);
    for (int i = 0; i < s_scan_count; i++) {
        con_printf("      %2d  %-24.24s %4d dBm  %02x:%02x:%02x:%02x:%02x:%02x  %s\r\n",
                   i + 1, (const char *)s_scan[i].ssid, s_scan[i].rssi,
                   s_scan[i].bssid[0], s_scan[i].bssid[1], s_scan[i].bssid[2],
                   s_scan[i].bssid[3], s_scan[i].bssid[4], s_scan[i].bssid[5],
                   auth_str(s_scan[i].authmode));
    }
    con_puts("    join <n> to stage a network\r\n(set|scan|list|use|save) # ");
}

static void scan_start(void)
{
    /* Asynchronous foreground scan: works while associated, and returning
     * immediately keeps the TinyUSB task (our caller) unblocked. */
    wifi_scan_config_t cfg = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        con_printf("[!] scan failed to start (%s)\r\n", esp_err_to_name(err));
        return;
    }
    s_scan_running = true;
    con_puts("[*] scanning...\r\n");
}

/* --- commands ----------------------------------------------------------- */

static void apply_active(void)
{
    con_puts("[*] applying -- re-associating\r\n");
    led_set_provisioned(active_profile()->ssid[0] != '\0');
    wifi_apply_creds(active_profile()->ssid, active_profile()->pass);
}

static void show_state(void)
{
    bridge_stats_t st;
    bridge_get_stats(&st);
    uint8_t mac[6];
    bridge_get_mac(mac);

    con_printf("    profiles:  %d saved (active: %d)\r\n", profile_count(),
               active_profile()->ssid[0] ? s_cfg.active + 1 : 0);
    con_printf("    ssid:      %s\r\n",
               active_profile()->ssid[0] ? active_profile()->ssid : "(unset)");
    con_printf("    pass:      %s\r\n", active_profile()->pass[0] ? "set" : "unset (open)");
    con_printf("    country:   %s\r\n", s_cfg.country[0]
               ? (strcmp(s_cfg.country, "01") == 0 ? "WORLDWIDE" : s_cfg.country)
               : "(unset)");
    con_printf("    debug:     %s\r\n", s_cfg.debug ? "on" : "off");
    con_printf("    status:    %s\r\n",
               bridge_wifi_connected() ? "associated"
               : (active_profile()->ssid[0] ? bridge_link_status() : "unprovisioned"));
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
    con_printf("    stats:     ->wifi=%lu ->host=%lu txdrop=%lu rxdrop=%lu refl=%lu poolfail=%lu\r\n",
               (unsigned long)st.host_to_wifi, (unsigned long)st.wifi_to_host,
               (unsigned long)st.txdrop, (unsigned long)st.rxdrop,
               (unsigned long)st.reflected, (unsigned long)st.poolfail);

    bridge_crash_info_t ci;
    bridge_get_crash(&ci);
    con_printf("    health:    boots=%lu hangs=%lu faults=%lu\r\n",
               (unsigned long)ci.boots, (unsigned long)ci.hangs, (unsigned long)ci.faults);
    if (ci.recovered) {
        con_printf("    RECOVERED from %s this boot; pre-crash ->wifi=%lu ->host=%lu "
                   "txdrop=%lu refl=%lu poolfail=%lu\r\n", ci.recovered,
                   (unsigned long)ci.pre.host_to_wifi, (unsigned long)ci.pre.wifi_to_host,
                   (unsigned long)ci.pre.txdrop, (unsigned long)ci.pre.reflected,
                   (unsigned long)ci.pre.poolfail);
    }
}

static void list_profiles(void)
{
    con_printf("    profiles (%d/%d):\r\n", profile_count(), MAX_PROFILES);
    for (int i = 0; i < MAX_PROFILES; i++) {
        if (s_cfg.p[i].ssid[0]) {
            con_printf("      %d%c %s%s\r\n", i + 1, i == s_cfg.active ? '*' : ' ',
                       s_cfg.p[i].ssid, s_cfg.p[i].pass[0] ? "" : "  (open)");
        }
    }
    con_puts("    (* = active)\r\n");
}

/* Stage `ssid` as the active profile: reuse a profile that already has this
 * SSID, else the active slot if empty, else the first free slot. */
static void stage_ssid(const char *ssid)
{
    int slot = -1;
    for (int i = 0; i < MAX_PROFILES; i++) {
        if (strcmp(s_cfg.p[i].ssid, ssid) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0 && !active_profile()->ssid[0]) {
        slot = s_cfg.active;
    }
    if (slot < 0) {
        for (int i = 0; i < MAX_PROFILES; i++) {
            if (!s_cfg.p[i].ssid[0]) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        con_puts("[!] all profile slots full; del one first\r\n");
        return;
    }
    if (strcmp(s_cfg.p[slot].ssid, ssid) != 0) {
        strlcpy(s_cfg.p[slot].ssid, ssid, sizeof(s_cfg.p[slot].ssid));
        s_cfg.p[slot].pass[0] = '\0';
    }
    s_cfg.active = slot;
    apply_active();
}

static void handle_line(char *line)
{
    while (*line == ' ') {
        line++;
    }
    if (line[0] == '\0') {
        return;
    }

    if (strcmp(line, "help") == 0) {
        con_puts("    set ssid <text>    set the active profile's SSID and re-associate\r\n"
                 "    set pass <text>    set its passphrase (blank for open) and re-associate\r\n"
                 "    set country <CC|WORLDWIDE>  set the regulatory country\r\n"
                 "    set led <gpio>     move the WS2812 status LED pin (38 on v1.1 devkits)\r\n"
                 "    set debug <on|off> stream diagnostics on this console\r\n"
                 "    show               show state\r\n"
                 "    scan               scan for networks; join <n> stages one\r\n"
                 "    list               list saved profiles\r\n"
                 "    use <n>            make profile n active and re-associate\r\n"
                 "    del <n>            delete profile n\r\n"
                 "    save               persist profiles and settings to NVS\r\n"
                 "    clear              erase everything saved\r\n"
                 "    reboot             restart the device\r\n");
    } else if (strcmp(line, "show") == 0) {
        show_state();
    } else if (strcmp(line, "list") == 0) {
        list_profiles();
    } else if (strcmp(line, "scan") == 0) {
        scan_start();
    } else if (strncmp(line, "join ", 5) == 0) {
        int n = atoi(line + 5);
        if (n < 1 || n > s_scan_count) {
            con_puts("[!] no such scan entry (run scan first)\r\n");
        } else {
            stage_ssid((const char *)s_scan[n - 1].ssid);
            if (s_scan[n - 1].authmode != WIFI_AUTH_OPEN && !active_profile()->pass[0]) {
                con_puts("[*] staged -- set pass <password> then save\r\n");
            }
        }
    } else if (strncmp(line, "use ", 4) == 0) {
        int n = atoi(line + 4);
        if (n < 1 || n > MAX_PROFILES || !s_cfg.p[n - 1].ssid[0]) {
            con_puts("[!] no such profile\r\n");
        } else {
            s_cfg.active = n - 1;
            apply_active();
        }
    } else if (strncmp(line, "del ", 4) == 0) {
        int n = atoi(line + 4);
        if (n < 1 || n > MAX_PROFILES || !s_cfg.p[n - 1].ssid[0]) {
            con_puts("[!] no such profile\r\n");
        } else {
            memset(&s_cfg.p[n - 1], 0, sizeof(profile_t));
            if (s_cfg.active == n - 1) { /* deleted the active one: pick first used */
                s_cfg.active = 0;
                for (int i = 0; i < MAX_PROFILES; i++) {
                    if (s_cfg.p[i].ssid[0]) {
                        s_cfg.active = i;
                        break;
                    }
                }
                apply_active();
            }
            con_puts("[*] deleted (save to persist)\r\n");
        }
    } else if (strncmp(line, "set ssid", 8) == 0 && (line[8] == ' ' || line[8] == '\0')) {
        strlcpy(active_profile()->ssid, line[8] ? line + 9 : "",
                sizeof(active_profile()->ssid));
        apply_active();
    } else if (strncmp(line, "set pass", 8) == 0 && (line[8] == ' ' || line[8] == '\0')) {
        strlcpy(active_profile()->pass, line[8] ? line + 9 : "",
                sizeof(active_profile()->pass));
        apply_active();
    } else if (strncmp(line, "set led ", 8) == 0) {
        int gpio = atoi(line + 8);
        if (gpio < 0 || gpio > 48) {
            con_puts("[!] usage: set led <gpio 0-48>\r\n");
        } else if (!led_set_gpio(gpio)) {
            con_printf("[!] could not drive GPIO%d\r\n", gpio);
        } else {
            /* persist immediately: hardware config, not a credential edit */
            nvs_handle_t h;
            if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_u8(h, "ledgpio", (uint8_t)gpio);
                nvs_commit(h);
                nvs_close(h);
            }
            con_printf("[*] LED on GPIO%d (persisted) -- solid/blinking if that's the right pin\r\n", gpio);
        }
    } else if (strncmp(line, "set country ", 12) == 0) {
        const char *cc = line + 12;
        if (strcasecmp(cc, "WORLDWIDE") == 0) {
            cc = "01";
        }
        if (strlen(cc) != 2) {
            con_puts("[!] usage: set country <CC|WORLDWIDE> (two-letter code)\r\n");
        } else {
            s_cfg.country[0] = toupper((unsigned char)cc[0]);
            s_cfg.country[1] = toupper((unsigned char)cc[1]);
            s_cfg.country[2] = '\0';
            esp_err_t err = esp_wifi_set_country_code(s_cfg.country, true);
            con_printf(err == ESP_OK ? "[*] country set (save to persist)\r\n"
                                     : "[!] rejected: %s\r\n", esp_err_to_name(err));
        }
    } else if (strcmp(line, "set debug on") == 0) {
        dbg_set(true);
        con_puts("[*] debug stream on (save to persist)\r\n");
    } else if (strcmp(line, "set debug off") == 0) {
        dbg_set(false);
        con_puts("[*] debug stream off\r\n");
    } else if (strcmp(line, "save") == 0) {
        con_puts(cfg_save() == ESP_OK ? "[*] saved to NVS\r\n" : "[!] save failed\r\n");
    } else if (strcmp(line, "clear") == 0) {
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
        con_puts("[*] saved config erased (running config unchanged)\r\n");
    } else if (strcmp(line, "reboot") == 0) {
        con_puts("[*] rebooting\r\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else {
        con_puts("[!] unknown command; try help\r\n");
    }
}

/* --- CDC plumbing ------------------------------------------------------- */

static void prompt(void)
{
    con_puts("(set|scan|list|use|save) # ");
}

static void banner(void)
{
    con_puts("\r\n-- esp32-usb-eth --\r\n");
    show_state();
    prompt();
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
                    prompt(); /* bare Enter: reprint the prompt (also lets tools probe for the console) */
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
    /* creds_load() (called by app_main before this) already loaded s_cfg */

    const tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = cdc_rx_cb,
        .callback_line_state_changed = cdc_line_state_cb,
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                               scan_done_handler, NULL));

    const esp_timer_create_args_t targs = {
        .callback = dbg_timer_cb,
        .name = "dbgstats",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_dbg_timer));
    if (s_cfg.debug) {
        dbg_set(true);
    }
    ESP_LOGI(TAG, "management console on CDC-ACM 0");
}
