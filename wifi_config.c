#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "router_config.h"
#include "wifi_config.h"

char *ssid = NULL;
char *passwd = NULL;
char *ap_ssid = NULL;
char *ap_passwd = NULL;
char *admin_user = NULL;
char *admin_pass = NULL;
static bool wifi_scan_active = false;

static const char *TAG = "wifi_config";
static SemaphoreHandle_t cfg_mutex = NULL;
static portMUX_TYPE scan_mux = portMUX_INITIALIZER_UNLOCKED;

void wifi_config_init(void)
{
    /* Called once, before any other task exists - see the header
     * comment. If it somehow runs twice (it shouldn't), the second
     * call is a harmless no-op. */
    if (!cfg_mutex) {
        cfg_mutex = xSemaphoreCreateMutex();
    }
}

void wifi_config_lock(void)
{
    /* Defensive fallback only: by the time any task other than
     * app_main can call this, wifi_config_init() has already run. */
    if (!cfg_mutex) cfg_mutex = xSemaphoreCreateMutex();
    if (cfg_mutex) xSemaphoreTake(cfg_mutex, portMAX_DELAY);
}

void wifi_config_unlock(void)
{
    if (cfg_mutex) xSemaphoreGive(cfg_mutex);
}

bool wifi_scan_try_begin(void)
{
    bool started = false;
    portENTER_CRITICAL(&scan_mux);
    if (!wifi_scan_active) {
        wifi_scan_active = true;
        started = true;
    }
    portEXIT_CRITICAL(&scan_mux);
    return started;
}

void wifi_scan_end(void)
{
    portENTER_CRITICAL(&scan_mux);
    wifi_scan_active = false;
    portEXIT_CRITICAL(&scan_mux);
}

bool wifi_scan_is_active(void)
{
    bool active;
    portENTER_CRITICAL(&scan_mux);
    active = wifi_scan_active;
    portEXIT_CRITICAL(&scan_mux);
    return active;
}

static char *dup_or_empty(const char *s)
{
    char *p = strdup(s ? s : "");
    if (!p) ESP_LOGE(TAG, "out of memory");
    return p;
}

static esp_err_t nvs_get_string(const char *key, char **value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PARAM_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;

    size_t len = 0;
    err = nvs_get_str(nvs, key, NULL, &len);
    if (err == ESP_OK) {
        *value = malloc(len);
        if (!*value) {
            err = ESP_ERR_NO_MEM;
        } else {
            err = nvs_get_str(nvs, key, *value, &len);
            if (err != ESP_OK) {
                free(*value);
                *value = NULL;
            }
        }
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t nvs_set_two_strings(const char *key1, const char *value1,
                                     const char *key2, const char *value2)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_set_str(nvs, key1, value1);
    if (err == ESP_OK) err = nvs_set_str(nvs, key2, value2);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t wifi_config_load(void)
{
    wifi_config_lock();
    wifi_config_free();

    if (nvs_get_string("ssid", &ssid) != ESP_OK) ssid = dup_or_empty("");
    if (nvs_get_string("passwd", &passwd) != ESP_OK) passwd = dup_or_empty("");
    if (nvs_get_string("ap_ssid", &ap_ssid) != ESP_OK) ap_ssid = dup_or_empty(DEFAULT_AP_SSID);
    if (nvs_get_string("ap_passwd", &ap_passwd) != ESP_OK) ap_passwd = dup_or_empty(DEFAULT_AP_PASSWORD);
    /* No default admin credentials are ever compiled in: an empty
     * admin_pass means "setup not completed yet" and is handled
     * explicitly by the HTTP layer (see wifi_config_admin_configured). */
    if (nvs_get_string("admin_user", &admin_user) != ESP_OK) admin_user = dup_or_empty("");
    if (nvs_get_string("admin_pass", &admin_pass) != ESP_OK) admin_pass = dup_or_empty("");

    bool ok = ssid && passwd && ap_ssid && ap_passwd && admin_user && admin_pass;
    wifi_config_unlock();
    return ok ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t wifi_config_save_sta(const char *new_ssid, const char *new_passwd)
{
    if (!new_ssid || !new_passwd || strlen(new_ssid) == 0 || strlen(new_ssid) > 32 || strlen(new_passwd) > 64) {
        return ESP_ERR_INVALID_ARG;
    }

    char *s = dup_or_empty(new_ssid);
    char *p = dup_or_empty(new_passwd);
    if (!s || !p) {
        free(s);
        free(p);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nvs_set_two_strings("ssid", new_ssid, "passwd", new_passwd);
    if (err == ESP_OK) {
        wifi_config_lock();
        free(ssid);
        free(passwd);
        ssid = s;
        passwd = p;
        wifi_config_unlock();
    } else {
        free(s);
        free(p);
    }
    return err;
}

esp_err_t wifi_config_save_ap(const char *new_ssid, const char *new_passwd)
{
    if (!new_ssid || !new_passwd || strlen(new_ssid) == 0 || strlen(new_ssid) > 32 || strlen(new_passwd) > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    if (new_passwd[0] && strlen(new_passwd) < 8) return ESP_ERR_INVALID_ARG;

    char *s = dup_or_empty(new_ssid);
    char *p = dup_or_empty(new_passwd);
    if (!s || !p) {
        free(s);
        free(p);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nvs_set_two_strings("ap_ssid", new_ssid, "ap_passwd", new_passwd);
    if (err == ESP_OK) {
        wifi_config_lock();
        free(ap_ssid);
        free(ap_passwd);
        ap_ssid = s;
        ap_passwd = p;
        wifi_config_unlock();
    } else {
        free(s);
        free(p);
    }
    return err;
}

esp_err_t wifi_config_save_admin(const char *new_user, const char *new_pass)
{
    if (!new_user || !new_pass || strlen(new_user) == 0 || strlen(new_user) > 32) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(new_pass) < 8 || strlen(new_pass) > 64) return ESP_ERR_INVALID_ARG;

    char *u = dup_or_empty(new_user);
    char *p = dup_or_empty(new_pass);
    if (!u || !p) {
        free(u);
        free(p);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nvs_set_two_strings("admin_user", new_user, "admin_pass", new_pass);
    if (err == ESP_OK) {
        wifi_config_lock();
        free(admin_user);
        free(admin_pass);
        admin_user = u;
        admin_pass = p;
        wifi_config_unlock();
    } else {
        free(u);
        free(p);
    }
    return err;
}

bool wifi_config_admin_configured(void)
{
    wifi_config_lock();
    bool configured = admin_pass && admin_pass[0] != '\0';
    wifi_config_unlock();
    return configured;
}

esp_err_t wifi_config_apply_ap(void)
{
    wifi_config_t cfg = {0};
    wifi_config_lock();
    strlcpy((char *)cfg.ap.ssid, ap_ssid, sizeof(cfg.ap.ssid));
    strlcpy((char *)cfg.ap.password, ap_passwd, sizeof(cfg.ap.password));
    cfg.ap.ssid_len = strlen(ap_ssid);
    cfg.ap.authmode = ap_passwd[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config_unlock();
    cfg.ap.channel = 1;
    cfg.ap.max_connection = AP_MAX_CONNECTIONS;
    cfg.ap.beacon_interval = 100;
    cfg.ap.pmf_cfg.required = false;
    return esp_wifi_set_config(WIFI_IF_AP, &cfg);
}

esp_err_t wifi_config_apply_sta(void)
{
    wifi_config_t cfg = {0};
    wifi_config_lock();
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, passwd, sizeof(cfg.sta.password));
    wifi_config_unlock();
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    return esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

void wifi_config_free(void)
{
    free(ssid);
    free(passwd);
    free(ap_ssid);
    free(ap_passwd);
    free(admin_user);
    free(admin_pass);
    ssid = NULL;
    passwd = NULL;
    ap_ssid = NULL;
    ap_passwd = NULL;
    admin_user = NULL;
    admin_pass = NULL;
}
