/* Shared interface between the bridge core (tusb_ncm_main.c) and the
 * management console (console.c). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t host_to_wifi;  /* frames forwarded host -> Wi-Fi */
    uint32_t wifi_to_host;  /* frames forwarded Wi-Fi -> host */
    uint32_t txdrop;        /* host -> Wi-Fi dropped (not associated) */
    uint32_t reflected;     /* Wi-Fi -> host dropped (host's own frame echoed by the AP) */
    uint32_t poolfail;      /* host -> Wi-Fi dropped (driver out of TX buffers) */
} bridge_stats_t;

void bridge_get_stats(bridge_stats_t *s);
void bridge_get_mac(uint8_t mac[6]);
bool bridge_wifi_connected(void);

/* "up" when associated; otherwise the last failure reason mapped pico-style:
 * "badauth" (wrong passphrase), "nonet" (SSID not found), or "join". */
const char *bridge_link_status(void);

/* Host addresses snooped passively from host -> Wi-Fi frames (the bridge holds
 * no IP of its own). Return false while nothing has been seen yet. */
bool bridge_host_ipv4(uint8_t ip[4]);
bool bridge_host_ipv6(uint8_t ip[16]);

/* Drop any association and re-join with these credentials (empty ssid: just
 * disassociate). Safe to call from the console (TinyUSB task) context. */
void wifi_apply_creds(const char *ssid, const char *pass);

/* console.c */
void console_init(void);
/* Effective boot credentials: NVS if provisioned, else compile-time defaults. */
void creds_load(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz);
/* Diagnostics line on the console's debug stream; no-op unless debug is on. */
void console_debug_printf(const char *fmt, ...);
