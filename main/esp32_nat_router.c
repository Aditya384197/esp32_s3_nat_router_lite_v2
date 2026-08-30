#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/lwip_napt.h"
#include "lwip/ip_addr.h"
#include "router_config.h"
#include "wifi_config.h"
#include "http_server.h"

#if !IP_NAPT
#error "IP_NAPT must be enabled"
#endif

uint64_t sta_bytes_sent = 0;
uint64_t sta_bytes_received = 0;
uint16_t connect_count = 0;
bool ap_connect = false;
uint32_t my_ip = 0;
uint32_t my_ap_ip = 0;

esp_netif_t *wifiAP = NULL;
esp_netif_t *wifiSTA = NULL;

static const char *TAG = "ESP32S3-NAT";
static int64_t boot_time_us;
static esp_timer_handle_t reconnect_timer;
static uint32_t reconnect_delay_ms = 1000;
static volatile bool reconnect_pending = false;

#define RECONNECT_INITIAL_MS 1000U
#define RECONNECT_MAX_MS     30000U

static void ota_confirm_running_image(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;

    if (running &&
        esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
        ESP_LOGI(TAG, "OTA image confirmed valid");
    }
}

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

uint32_t get_uptime_seconds(void)
{
    return (uint32_t)((esp_timer_get_time() - boot_time_us) / 1000000ULL);
}

void format_uptime(uint32_t seconds, char *buf, size_t len)
{
    uint32_t d = seconds / 86400U;
    uint32_t h = (seconds % 86400U) / 3600U;
    uint32_t m = (seconds % 3600U) / 60U;
    uint32_t s = seconds % 60U;

    if (d) {
        snprintf(buf, len, "%" PRIu32 "d %02" PRIu32 "h %02" PRIu32 "m", d, h, m);
    } else {
        snprintf(buf, len, "%02" PRIu32 "h %02" PRIu32 "m %02" PRIu32 "s", h, m, s);
    }
}

static void set_ap_dns(uint32_t addr)
{
    esp_netif_dns_info_t dns = {0};
    dns.ip.u_addr.ip4.addr = addr;
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(wifiAP, ESP_NETIF_DNS_MAIN, &dns);
}

static void apply_ap_dns_from_sta(void)
{
    esp_netif_dns_info_t dns = {0};
    if (esp_netif_get_dns_info(wifiSTA, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
        dns.ip.type == ESP_IPADDR_TYPE_V4 && dns.ip.u_addr.ip4.addr != 0) {
        esp_netif_set_dns_info(wifiAP, ESP_NETIF_DNS_MAIN, &dns);
    }
}

/* ssid is read here from several different task contexts (system event
 * task, the reconnect esp_timer callback, and indirectly the HTTP
 * worker task via router_reconnect_uplink) while wifi_config_save_sta()
 * can free()+reassign it from the HTTP task at any time - so every read
 * goes through the config mutex rather than dereferencing the pointer
 * directly. */
static bool sta_ssid_configured(void)
{
    wifi_config_lock();
    bool has = ssid && ssid[0];
    wifi_config_unlock();
    return has;
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    reconnect_pending = false;
    if (sta_ssid_configured() && !wifi_scan_is_active() && !ap_connect) {
        esp_wifi_connect();
    }

    if (reconnect_delay_ms < RECONNECT_MAX_MS) {
        reconnect_delay_ms *= 2U;
        if (reconnect_delay_ms > RECONNECT_MAX_MS) {
            reconnect_delay_ms = RECONNECT_MAX_MS;
        }
    }
}

static void schedule_reconnect(void)
{
    if (!sta_ssid_configured() || wifi_scan_is_active() || ap_connect) return;
    esp_timer_stop(reconnect_timer);
    reconnect_pending = true;
    esp_timer_start_once(reconnect_timer, (uint64_t)reconnect_delay_ms * 1000ULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        reconnect_delay_ms = RECONNECT_INITIAL_MS;
        if (sta_ssid_configured()) {
            esp_wifi_connect();
        }
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ap_connect = false;
        my_ip = 0;
        set_ap_dns(my_ap_ip);
        if (!wifi_scan_is_active()) {
            schedule_reconnect();
        }
        captive_portal_start();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        wifi_scan_end();
        if (!ap_connect && sta_ssid_configured()) {
            schedule_reconnect();
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        my_ip = event->ip_info.ip.addr;
        ap_connect = true;
        reconnect_delay_ms = RECONNECT_INITIAL_MS;
        reconnect_pending = false;
        esp_timer_stop(reconnect_timer);
        apply_ap_dns_from_sta();
        ip_napt_enable(my_ap_ip, 1);
        init_byte_counter();
        ESP_LOGI(TAG, "uplink connected: " IPSTR, IP2STR(&event->ip_info.ip));
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        ip_napt_enable(my_ap_ip, 1);
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        if (connect_count < AP_MAX_CONNECTIONS) connect_count++;
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (connect_count) connect_count--;
    }
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifiAP = esp_netif_create_default_wifi_ap();
    wifiSTA = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(wifiAP ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(wifiSTA ? ESP_OK : ESP_ERR_NO_MEM);

    esp_netif_ip_info_t ap_ip = {0};
    ap_ip.ip.addr = esp_ip4addr_aton(DEFAULT_AP_IP);
    ap_ip.gw.addr = ap_ip.ip.addr;
    ap_ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(wifiAP));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(wifiAP, &ap_ip));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(wifiAP));
    my_ap_ip = ap_ip.ip.addr;
    set_ap_dns(my_ap_ip);

    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .name = "sta_reconnect"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &reconnect_timer));

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(wifi_config_apply_ap());
    ESP_ERROR_CHECK(wifi_config_apply_sta());
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Modem-sleep power save is ON by default and adds latency to every
     * RX/TX, which caps NAT throughput well below what the radio can do.
     * A mains-powered router has no reason to save power at the expense
     * of forwarding speed. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    wifi_config_lock();
    ESP_LOGI(TAG, "AP: %s / http://%s", ap_ssid, DEFAULT_AP_IP);
    wifi_config_unlock();
}

void router_reconnect_uplink(void)
{
    reconnect_delay_ms = RECONNECT_INITIAL_MS;
    reconnect_pending = false;
    esp_timer_stop(reconnect_timer);
    wifi_scan_end();
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(wifi_config_apply_sta());
    if (sta_ssid_configured()) {
        esp_wifi_connect();
    }
}

esp_err_t router_apply_ap_config(void)
{
    /* Deliberately AP-only: esp_wifi_set_config(WIFI_IF_AP, ...) is the
     * standard way to change SSID/password on a running softAP and
     * takes effect immediately (existing AP-side clients get dropped
     * and must reconnect with the new credentials, which is expected).
     * A full esp_wifi_stop()/esp_wifi_start() cycle was tried here at
     * one point to "guarantee" the new settings apply, but in
     * WIFI_MODE_APSTA that stops BOTH interfaces - it would force the
     * STA internet uplink to fully disconnect and reassociate (with a
     * DHCP lease renewal) every time someone just renames the AP or
     * changes its password, which is a real, guaranteed outage on a
     * router whose main job is an uninterrupted uplink. Not worth it
     * for a theoretical edge case; keep this scoped to WIFI_IF_AP only. */
    return wifi_config_apply_ap();
}

void app_main(void)
{
    wifi_config_init();
    boot_time_us = esp_timer_get_time();
    nvs_init();
    ESP_ERROR_CHECK(wifi_config_load());
    ota_confirm_running_image();
    wifi_start();
    if (!sta_ssid_configured()) captive_portal_start();
    ESP_ERROR_CHECK(start_webserver(80) ? ESP_OK : ESP_FAIL);
    ESP_LOGI(TAG, "ESP32-S3 NAT router ready");
    if (!wifi_config_admin_configured()) {
        ESP_LOGW(TAG, "No admin password set yet - open http://%s to finish setup", DEFAULT_AP_IP);
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
