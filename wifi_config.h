#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

extern char *ssid;
extern char *passwd;
extern char *ap_ssid;
extern char *ap_passwd;
extern char *admin_user;
extern char *admin_pass;
/* wifi_scan_active is intentionally NOT exposed here anymore - it is
 * private to wifi_config.c. Every reader/writer must go through
 * wifi_scan_try_begin()/wifi_scan_end()/wifi_scan_is_active() below so
 * the spinlock protecting it can't be silently bypassed by future code
 * that touches the raw bool directly. */

esp_err_t wifi_config_load(void);
esp_err_t wifi_config_save_sta(const char *new_ssid, const char *new_passwd);
esp_err_t wifi_config_save_ap(const char *new_ssid, const char *new_passwd);
esp_err_t wifi_config_save_admin(const char *new_user, const char *new_pass);
bool wifi_config_admin_configured(void);
esp_err_t wifi_config_apply_ap(void);
esp_err_t wifi_config_apply_sta(void);
void wifi_config_free(void);

/* Must be called exactly once, as the very first thing in app_main,
 * before any other task (event loop, httpd, timers) can exist. This
 * removes the lazy "create on first use" pattern, which could in
 * theory race if two tasks called wifi_config_lock() for the first
 * time concurrently - calling this deterministically before any other
 * task is spawned makes that race structurally impossible rather than
 * just unlikely. */
void wifi_config_init(void);

/* Guards ssid/passwd/ap_ssid/ap_passwd/admin_user/admin_pass against a
 * reader task dereferencing a pointer that a writer task is freeing and
 * reassigning at the same time. Hold for the shortest span possible. */
void wifi_config_lock(void);
void wifi_config_unlock(void);

/* Serialize Wi-Fi scans across HTTP, event, and reconnect contexts. */
bool wifi_scan_try_begin(void);
void wifi_scan_end(void);
bool wifi_scan_is_active(void);

#ifdef __cplusplus
}
#endif
