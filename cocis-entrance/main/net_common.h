#ifndef NET_COMMON_H
#define NET_COMMON_H

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"

// CHANGE THIS: Unique identifier so your team's network data doesn't mix in the lab
#define TOPIC_ROOT "makerere/cocis/group_2026_final"

// Public MQTT broker (a host name, not an IP address that may change)
#define MQTT_BROKER_URI "mqtt://broker.hivemq.com"

// MQTT Topic Definitions
#define TOPIC_ROOM1_STATUS   TOPIC_ROOT "/room1/status"
#define TOPIC_ROOM1_STATS    TOPIC_ROOT "/room1/stats"
#define TOPIC_ROOM1_CMD      TOPIC_ROOT "/room1/cmd"
#define TOPIC_ROOM2_STATUS   TOPIC_ROOT "/room2/status"
#define TOPIC_ROOM2_STATS    TOPIC_ROOT "/room2/stats"
#define TOPIC_ROOM2_CMD      TOPIC_ROOT "/room2/cmd"
#define TOPIC_SYSTEM_TIME    TOPIC_ROOT "/system/time"
#define TOPIC_ALERTS         TOPIC_ROOT "/system/alerts"

// Shared Status Signals
#define PAYLOAD_OCCUPIED "OCCUPIED"
#define PAYLOAD_VACANT   "VACANT"
#define PAYLOAD_OFFLINE  "OFFLINE"     // sent by the broker if a room node dies (last will)

// ---------------- WiFi (Wokwi simulated network) ----------------
static EventGroupHandle_t wifi_event_group;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();                              // try again
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, 1);         // we have an IP address
    }
}

// Returns 1 when connected (waits up to 20 s). nvs_flash_init() must be called before.
static int wifi_start(void) {
    wifi_event_group = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&init_cfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    wifi_config_t wifi_cfg = { 0 };
    strcpy((char *)wifi_cfg.sta.ssid, "Wokwi-GUEST");    // open network, no password
    wifi_cfg.sta.channel = 6;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start();

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, 1, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    return (bits & 1) ? 1 : 0;
}

#endif // NET_COMMON_H
