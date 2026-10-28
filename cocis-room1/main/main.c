#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "net_common.h"

// Set this to 1 for Room 1, or 2 for Room 2
#define ROOM_NUMBER      1

#if ROOM_NUMBER == 1
  #define ROOM_ID          "ROOM_1"
  #define MY_STATUS_TOPIC  TOPIC_ROOM1_STATUS
  #define MY_STATS_TOPIC   TOPIC_ROOM1_STATS
  #define MY_CMD_TOPIC     TOPIC_ROOM1_CMD
  #define MY_CLIENT_ID     "cocis_group2026_room1"
#else
  #define ROOM_ID          "ROOM_2"
  #define MY_STATUS_TOPIC  TOPIC_ROOM2_STATUS
  #define MY_STATS_TOPIC   TOPIC_ROOM2_STATS
  #define MY_CMD_TOPIC     TOPIC_ROOM2_CMD
  #define MY_CLIENT_ID     "cocis_group2026_room2"
#endif

#define PIR_INPUT_PIN    GPIO_NUM_13
#define RELAY_LED_PIN    GPIO_NUM_12
#define TIMEOUT_SECONDS  20
#define REPORT_SECONDS   5       // status + statistics are repeated every 5 s (heartbeat)

static const char *TAG = "ROOM_CLIENT_NODE";
esp_mqtt_client_handle_t mqtt_client = NULL;
bool mqtt_connected = false;
volatile int light_mode = 0;              // 0 = auto, 1 = forced ON, 2 = forced OFF (from entrance)
uint32_t active_wasted_seconds = 0;       // light ON while the room was empty
uint32_t cumulative_on_seconds = 0;       // total time the light was ON

void init_room_peripherals() {
    gpio_reset_pin(PIR_INPUT_PIN);
    gpio_set_direction(PIR_INPUT_PIN, GPIO_MODE_INPUT);
    gpio_pullup_en(PIR_INPUT_PIN);

    gpio_reset_pin(RELAY_LED_PIN);
    gpio_set_direction(RELAY_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(RELAY_LED_PIN, 0);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            mqtt_connected = true;
            esp_mqtt_client_subscribe(mqtt_client, MY_CMD_TOPIC, 1);
            ESP_LOGI(TAG, "%s connected to MQTT", ROOM_ID);
            break;
        case MQTT_EVENT_DISCONNECTED:
            mqtt_connected = false;
            break;
        case MQTT_EVENT_DATA: {
            char cmd[8] = "";
            int n = event->data_len < 7 ? event->data_len : 7;
            memcpy(cmd, event->data, n);
            if (strcmp(cmd, "ON") == 0)   light_mode = 1;
            if (strcmp(cmd, "OFF") == 0)  light_mode = 2;
            if (strcmp(cmd, "AUTO") == 0) light_mode = 0;
            ESP_LOGI(TAG, "Command from entrance: %s", cmd);
            break;
        }
        default:
            break;
    }
}

void room_automation_loop(void *pvParameters) {
    int countdown_timer = 0;
    int last_occupied = -1;               // -1 forces a first report
    int seconds_since_report = 0;

    while (1) {
        int input_motion = gpio_get_level(PIR_INPUT_PIN);

        // Every motion restarts the countdown, so the light does not flicker
        if (input_motion == 1) {
            countdown_timer = TIMEOUT_SECONDS;
        } else if (countdown_timer > 0) {
            countdown_timer--;
        }
        int occupied = (countdown_timer > 0) ? 1 : 0;

        // Automatic: light follows the room. Or the entrance forces it ON / OFF.
        int light = (light_mode == 1) ? 1 : (light_mode == 2) ? 0 : occupied;
        gpio_set_level(RELAY_LED_PIN, light);

        if (light) {
            cumulative_on_seconds++;
            if (!occupied) active_wasted_seconds++;     // energy wasted
        }

        if (occupied != last_occupied) {
            ESP_LOGI(TAG, "%s", occupied ? "Motion detected! Lighting array enabled."
                                         : "Timeout threshold met. Extinguishing grid array power.");
        }

        // Report when the state changes, and every REPORT_SECONDS as a heartbeat
        seconds_since_report++;
        if (mqtt_connected && (occupied != last_occupied || seconds_since_report >= REPORT_SECONDS)) {
            esp_mqtt_client_publish(mqtt_client, MY_STATUS_TOPIC, occupied ? PAYLOAD_OCCUPIED : PAYLOAD_VACANT, 0, 1, 1);

            char stats_payload[128];
            snprintf(stats_payload, sizeof(stats_payload), "WastedTime: %lus | TotalDurationOnTime: %lus",
                     (unsigned long)active_wasted_seconds, (unsigned long)cumulative_on_seconds);
            esp_mqtt_client_publish(mqtt_client, MY_STATS_TOPIC, stats_payload, 0, 1, 1);

            seconds_since_report = 0;
            last_occupied = occupied;
        }
        if (!mqtt_connected) last_occupied = occupied;

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void) {
    esp_err_t r = nvs_flash_init();       // needed by WiFi
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    init_room_peripherals();

    // The light works locally even if the network fails
    xTaskCreate(room_automation_loop, "room_core_task", 4096, NULL, 5, NULL);

    if (wifi_start()) {
        esp_mqtt_client_config_t mqtt_cfg = {
            .broker.address.uri = MQTT_BROKER_URI,
            .credentials.client_id = MY_CLIENT_ID,
            // If this node dies, the broker tells the entrance: OFFLINE
            .session.last_will = {
                .topic = MY_STATUS_TOPIC,
                .msg = PAYLOAD_OFFLINE,
                .msg_len = 7,
                .qos = 1,
                .retain = 1
            }
        };
        mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
        esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        esp_mqtt_client_start(mqtt_client);
    } else {
        ESP_LOGW(TAG, "WiFi not connected: running standalone");
    }
}
