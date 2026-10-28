#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "net_common.h"

// ================= PIN DEFINITIONS =================
#define ENTRY_LED_PIN     GPIO_NUM_2      // green : entry door
#define EXIT_LED_PIN      GPIO_NUM_4      // blue  : exit door
#define DENIED_LED_PIN    GPIO_NUM_15     // red   : access denied
#define BUZZER_PIN        GPIO_NUM_13

#define SD_CS_PIN         5
#define SD_MOSI_PIN       23
#define SD_MISO_PIN       19
#define SD_SCK_PIN        18

// ================= SETTINGS =================
#define MAX_USERS         20
#define ADMIN_PIN         "1234"
#define ROOM_TIMEOUT_MS   15000           // room is OFFLINE if silent for 15 s
#define MAX_STAY_MINUTES  480             // 8 hours inside = overstay alert

typedef struct {
    char uid[16];
    char name[12];
    bool is_inside;
    uint32_t entry_time;                  // minutes since midnight
    int role;                             // 0: Student, 1: Staff, 2: Admin
    bool alerted;                         // overstay alert already sent
} UserRegistry;

static const char *TAG = "ENTRANCE_NODE";
const char *role_names[] = { "Student", "Staff", "Admin" };
const char *mode_names[] = { "auto", "on", "off" };

// ================= GLOBAL VARIABLES =================
SemaphoreHandle_t xStateMutex = NULL;     // protects users, rooms, clock, schedule
SemaphoreHandle_t xSdMutex = NULL;        // protects the SD card file
esp_mqtt_client_handle_t mqtt_client = NULL;
bool mqtt_connected = false;
bool sd_ready = false;

UserRegistry registry[MAX_USERS];
int registered_count = 0;

int sys_hour = 9;                         // software clock (see clock_task)
int sys_min = 0;
int sys_sec = 0;
int start_total = 8 * 60;                 // access allowed from 08:00 (minutes)
int end_total = 17 * 60;                  // until 17:00
bool emergency_exit = false;              // true = people may leave outside the schedule

bool is_logged_in = false;
int failed_login_attempts = 0;
TickType_t lock_until = 0;
TickType_t login_time = 0;

bool room_seen[2] = { false, false };
bool room_online[2] = { false, false };
bool room_occupied[2] = { false, false };
TickType_t room_last_seen[2] = { 0, 0 };
char room_stats[2][64] = { "", "" };
int room_mode[2] = { 0, 0 };              // 0 auto, 1 forced on, 2 forced off

// ================= HELPER FUNCTIONS =================

// Write a log line to the console AND to the SD card
void append_to_sd_card(const char *fmt, ...) {
    char text[100];
    char line[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    snprintf(line, sizeof(line), "%02d:%02d:%02d,%s", sys_hour, sys_min, sys_sec, text);
    printf("[SD_CARD_FS]: %s\n", line);

    if (sd_ready) {
        xSemaphoreTake(xSdMutex, portMAX_DELAY);
        FILE *f = fopen("/sdcard/access.csv", "a");
        if (f != NULL) {
            fprintf(f, "%s\n", line);
            fclose(f);
        }
        xSemaphoreGive(xSdMutex);
    }
}

void beep(int ms) {
    gpio_set_level(BUZZER_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level(BUZZER_PIN, 0);
}

// 1 if inside the allowed access period (call with xStateMutex held, or from a single task)
bool is_open_now(void) {
    int now = sys_hour * 60 + sys_min;
    if (start_total <= end_total) return (now >= start_total && now < end_total);
    return (now >= start_total || now < end_total);       // period passes midnight
}

int count_inside(void) {
    int count = 0;
    for (int i = 0; i < registered_count; i++) {
        if (registry[i].is_inside) count++;
    }
    return count;
}

// Alert = log to SD + publish on MQTT + buzzer
void send_alert(const char *msg) {
    append_to_sd_card("ALERT,%s", msg);
    ESP_LOGW(TAG, "%s", msg);
    if (mqtt_connected) {
        esp_mqtt_client_publish(mqtt_client, TOPIC_ALERTS, msg, 0, 1, 0);
    }
    gpio_set_level(DENIED_LED_PIN, 1);           // red LED on
    beep(300);
    vTaskDelay(pdMS_TO_TICKS(700));              // keep the LED on for about 1 second
    gpio_set_level(DENIED_LED_PIN, 0);
}

bool add_user(const char *uid, const char *name, int role) {
    for (int i = 0; i < registered_count; i++) {
        if (strcmp(registry[i].uid, uid) == 0) return false;      // already exists
    }
    if (registered_count >= MAX_USERS) return false;
    strncpy(registry[registered_count].uid, uid, 15);
    strncpy(registry[registered_count].name, name, 11);
    registry[registered_count].is_inside = false;
    registry[registered_count].entry_time = 0;
    registry[registered_count].role = role;
    registry[registered_count].alerted = false;
    registered_count++;
    return true;
}

// ================= HARDWARE INITIALISATION =================
void init_gpio(void) {
    gpio_num_t pins[4] = { ENTRY_LED_PIN, EXIT_LED_PIN, DENIED_LED_PIN, BUZZER_PIN };
    for (int i = 0; i < 4; i++) {
        gpio_reset_pin(pins[i]);
        gpio_set_direction(pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(pins[i], 0);
    }
}

void init_sd_card(void) {
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;

    spi_bus_config_t bus_config = {
        .mosi_io_num = SD_MOSI_PIN,
        .miso_io_num = SD_MISO_PIN,
        .sclk_io_num = SD_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000
    };
    if (spi_bus_initialize(host.slot, &bus_config, SPI_DMA_CH_AUTO) != ESP_OK) {
        printf("SPI bus init failed. Running without SD card.\n");
        return;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS_PIN;
    slot_config.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 3,
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_card_t *card;
    if (esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card) == ESP_OK) {
        sd_ready = true;
        printf("SD card mounted.\n");
    } else {
        printf("SD card not found. Running without SD card.\n");
    }
}

// ================= SOFTWARE CLOCK =================
void clock_task(void *pvParameters) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        sys_sec++;
        if (sys_sec >= 60) {
            sys_sec = 0;
            sys_min++;
            if (sys_min >= 60) {
                sys_min = 0;
                sys_hour++;
                if (sys_hour >= 24) sys_hour = 0;
            }
        }
    }
}

// ================= ACCESS CONTROL =================
void handle_uid_scan(const char *uid, bool is_entry) {
    int user_idx = -1;
    bool granted = false;
    char reason[32] = "";
    char name[12] = "unknown";
    int inside_now = 0;

    xSemaphoreTake(xStateMutex, portMAX_DELAY);

    for (int i = 0; i < registered_count; i++) {
        if (strcmp(registry[i].uid, uid) == 0) { user_idx = i; break; }
    }

    if (user_idx == -1) {
        strcpy(reason, "unregistered tag");            // unknown tags are NOT accepted
    } else {
        strcpy(name, registry[user_idx].name);
        bool open_now = is_open_now();
        if (is_entry && !open_now) {
            strcpy(reason, "outside schedule");
        } else if (!is_entry && !open_now && !emergency_exit) {
            strcpy(reason, "exit locked (schedule)");
        } else if (is_entry && registry[user_idx].is_inside) {
            strcpy(reason, "anti-passback: already inside");
        } else if (!is_entry && !registry[user_idx].is_inside) {
            strcpy(reason, "anti-passback: not inside");
        } else {
            granted = true;
            registry[user_idx].is_inside = is_entry;
            registry[user_idx].alerted = false;
            if (is_entry) registry[user_idx].entry_time = sys_hour * 60 + sys_min;
        }
    }
    inside_now = count_inside();

    xSemaphoreGive(xStateMutex);          // released BEFORE the delays below

    if (granted) {
        append_to_sd_card("%s,%s,%s,GRANTED,inside=%d", is_entry ? "ENTRY" : "EXIT", uid, name, inside_now);
        gpio_num_t led = is_entry ? ENTRY_LED_PIN : EXIT_LED_PIN;
        gpio_set_level(led, 1);           // open the door
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_set_level(led, 0);
        printf("[ACCESS]: %s gate unlocked for Tag %s (%s)\n", is_entry ? "Entry" : "Exit", uid, name);
    } else {
        append_to_sd_card("%s,%s,%s,DENIED,%s", is_entry ? "ENTRY" : "EXIT", uid, name, reason);
        if (strncmp(reason, "anti-passback", 13) == 0) {
            char ab[100];
            snprintf(ab, sizeof(ab), "ANTI-PASSBACK,Tag %s (%s) %s", uid, name, reason);
            if (mqtt_connected) esp_mqtt_client_publish(mqtt_client, TOPIC_ALERTS, ab, 0, 1, 0);
        }
        printf("[DENIED]: %s\n", reason);
        for (int i = 0; i < 3; i++) {
            gpio_set_level(DENIED_LED_PIN, 1);
            beep(150);
            gpio_set_level(DENIED_LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(150));
        }
    }
}

// ================= AUTHENTICATION =================
void do_login(const char *pin) {
    TickType_t now = xTaskGetTickCount();
    if (now < lock_until) {
        printf("[LOCKED]: too many wrong attempts, try again in a few seconds.\n");
        return;
    }
    if (strcmp(pin, ADMIN_PIN) == 0) {
        is_logged_in = true;
        failed_login_attempts = 0;
        login_time = now;
        printf("SUCCESS: Administrator logged in (valid for 60 seconds).\n");
        append_to_sd_card("ADMIN,login,OK");
    } else {
        failed_login_attempts++;
        printf("REJECTED: wrong PIN (%d/3)\n", failed_login_attempts);
        append_to_sd_card("ADMIN,login,FAILED");
        if (failed_login_attempts >= 3) {
            lock_until = now + pdMS_TO_TICKS(30000);       // locked for 30 s, then it opens again
            failed_login_attempts = 0;
            send_alert("SECURITY,3 wrong PIN attempts, login locked for 30 s");
        }
    }
}

// Every configuration command calls this first
bool require_admin(void) {
    if (is_logged_in && (xTaskGetTickCount() - login_time) > pdMS_TO_TICKS(60000)) {
        is_logged_in = false;                              // session expired
    }
    if (!is_logged_in) {
        printf("ERROR: Privilege check failed. Authenticate first with: login <PIN>\n");
        return false;
    }
    return true;
}

// ================= MQTT (rooms) =================
void send_room_command(int room, const char *mode_text) {   // room 0/1, "ON" / "OFF" / "AUTO"
    const char *topic = (room == 0) ? TOPIC_ROOM1_CMD : TOPIC_ROOM2_CMD;
    esp_mqtt_client_publish(mqtt_client, topic, mode_text, 0, 1, 0);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            mqtt_connected = true;
            ESP_LOGI(TAG, "MQTT Network Pipeline Online. Registering room listeners...");
            esp_mqtt_client_subscribe(mqtt_client, TOPIC_ROOM1_STATUS, 1);
            esp_mqtt_client_subscribe(mqtt_client, TOPIC_ROOM2_STATUS, 1);
            esp_mqtt_client_subscribe(mqtt_client, TOPIC_ROOM1_STATS, 1);
            esp_mqtt_client_subscribe(mqtt_client, TOPIC_ROOM2_STATS, 1);
            break;

        case MQTT_EVENT_DISCONNECTED:
            mqtt_connected = false;
            break;

        case MQTT_EVENT_DATA: {
            if (event->topic == NULL || event->data_len != event->total_data_len) break;

            char topic[96];
            char data[72];
            int tl = event->topic_len < 95 ? event->topic_len : 95;
            int dl = event->data_len < 71 ? event->data_len : 71;
            memcpy(topic, event->topic, tl);  topic[tl] = '\0';
            memcpy(data, event->data, dl);    data[dl] = '\0';

            int room = -1;
            bool is_status = false;
            if (strcmp(topic, TOPIC_ROOM1_STATUS) == 0)     { room = 0; is_status = true; }
            else if (strcmp(topic, TOPIC_ROOM1_STATS) == 0) { room = 0; }
            else if (strcmp(topic, TOPIC_ROOM2_STATUS) == 0){ room = 1; is_status = true; }
            else if (strcmp(topic, TOPIC_ROOM2_STATS) == 0) { room = 1; }

            if (room >= 0) {
                xSemaphoreTake(xStateMutex, portMAX_DELAY);     // short: only updates variables
                room_seen[room] = true;
                room_last_seen[room] = xTaskGetTickCount();
                if (is_status) {
                    if (strcmp(data, PAYLOAD_OFFLINE) == 0) {
                        room_online[room] = false;
                    } else {
                        room_online[room] = true;
                        room_occupied[room] = (strcmp(data, PAYLOAD_OCCUPIED) == 0);
                    }
                } else {
                    room_online[room] = true;
                    strncpy(room_stats[room], data, 63);
                    room_stats[room][63] = '\0';
                }
                xSemaphoreGive(xStateMutex);
            }
            break;
        }
        default:
            break;
    }
}

// ================= SECURITY MONITOR (every 5 s) =================
// Alerts: overstay (8 h), people inside after closing, motion in a room while
// the building is closed, and a room that stopped reporting.
void check_overstay_alerts(void) {
    static bool closing_alerted = false;
    static bool motion_alerted[2] = { false, false };
    static bool offline_alerted[2] = { false, false };
    char msgs[10][96];
    int n = 0;

    xSemaphoreTake(xStateMutex, portMAX_DELAY);
    bool open_now = is_open_now();
    int now_min = sys_hour * 60 + sys_min;
    int count = count_inside();

    for (int i = 0; i < registered_count; i++) {
        if (registry[i].is_inside && !registry[i].alerted) {
            int stay = now_min - (int)registry[i].entry_time;
            if (stay < 0) stay += 1440;
            if (stay > MAX_STAY_MINUTES && n < 10) {
                registry[i].alerted = true;
                snprintf(msgs[n++], 96, "OVERSTAY,Tag %s (%s) inside for over %d min", registry[i].uid, registry[i].name, MAX_STAY_MINUTES);
            }
        }
    }
    if (!open_now && count > 0 && !closing_alerted && n < 10) {
        closing_alerted = true;
        snprintf(msgs[n++], 96, "OVERSTAY,%d person(s) still inside after closing time", count);
    }
    if (open_now) closing_alerted = false;

    for (int r = 0; r < 2; r++) {
        bool online = room_seen[r] && room_online[r] &&
                      ((xTaskGetTickCount() - room_last_seen[r]) < pdMS_TO_TICKS(ROOM_TIMEOUT_MS));
        if (room_seen[r] && !online && !offline_alerted[r] && n < 10) {
            offline_alerted[r] = true;
            snprintf(msgs[n++], 96, "ROOM%d,OFFLINE (no heartbeat)", r + 1);
        }
        if (online) offline_alerted[r] = false;

        if (!open_now && online && room_occupied[r] && !motion_alerted[r] && n < 10) {
            motion_alerted[r] = true;
            snprintf(msgs[n++], 96, "ROOM%d,MOTION WHILE BUILDING CLOSED", r + 1);
        }
        if (!room_occupied[r]) motion_alerted[r] = false;
    }
    xSemaphoreGive(xStateMutex);

    for (int k = 0; k < n; k++) send_alert(msgs[k]);       // after the mutex is released
}

void monitor_task(void *pvParameters) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        check_overstay_alerts();
    }
}

// ================= SERIAL COMMANDS =================
void print_help(void) {
    printf("\n--- CoCIS Command Interface ---\n");
    printf("help\n");
    printf("scan in <UID>            simulate RFID scan at the ENTRY reader\n");
    printf("scan out <UID>           simulate RFID scan at the EXIT reader\n");
    printf("who                      people currently inside\n");
    printf("status                   time, schedule, occupancy, links\n");
    printf("rooms                    room occupancy, lights, energy statistics\n");
    printf("dump                     print the access log stored on the SD card\n");
    printf("login <PIN> | logout\n");
    printf("[admin] setschedule <h1> <m1> <h2> <m2>     e.g. setschedule 8 0 17 30\n");
    printf("[admin] settime <HH:MM>\n");
    printf("[admin] emergency on|off\n");
    printf("[admin] light <1|2> <on|off|auto>\n");
    printf("[admin] adduser <UID> <name> <role 0-2>\n");
}

void parse_serial_command(const char *cmd) {
    int a, b, c, d;

    if (strcmp(cmd, "help") == 0) {
        print_help();
    }
    else if (strncmp(cmd, "scan in ", 8) == 0) {
        handle_uid_scan(cmd + 8, true);
    }
    else if (strncmp(cmd, "scan out ", 9) == 0) {
        handle_uid_scan(cmd + 9, false);
    }
    else if (strcmp(cmd, "who") == 0) {
        xSemaphoreTake(xStateMutex, portMAX_DELAY);
        printf("\n--- Active Occupants Inside Building ---\n");
        for (int i = 0; i < registered_count; i++) {
            if (registry[i].is_inside) {
                printf(" -> %s | UID: %s | %s | entered %02d:%02d\n", registry[i].name, registry[i].uid,
                       role_names[registry[i].role], (int)(registry[i].entry_time / 60), (int)(registry[i].entry_time % 60));
            }
        }
        printf("Total inside: %d\n", count_inside());
        xSemaphoreGive(xStateMutex);
    }
    else if (strcmp(cmd, "status") == 0) {
        xSemaphoreTake(xStateMutex, portMAX_DELAY);
        printf("\n--- SYSTEM STATUS ---\n");
        printf("Time: %02d:%02d:%02d\n", sys_hour, sys_min, sys_sec);
        printf("Access window: %02d:%02d - %02d:%02d (%s)\n", start_total / 60, start_total % 60,
               end_total / 60, end_total % 60, is_open_now() ? "OPEN" : "CLOSED");
        printf("Emergency exit: %s\n", emergency_exit ? "ON" : "off");
        printf("People in building: %d\n", count_inside());
        printf("SD card: %s | MQTT: %s\n", sd_ready ? "ready" : "not available", mqtt_connected ? "connected" : "not connected");
        xSemaphoreGive(xStateMutex);
    }
    else if (strcmp(cmd, "rooms") == 0) {
        xSemaphoreTake(xStateMutex, portMAX_DELAY);
        for (int r = 0; r < 2; r++) {
            if (!room_seen[r]) { printf("Room %d: no data yet\n", r + 1); continue; }
            bool online = room_online[r] && ((xTaskGetTickCount() - room_last_seen[r]) < pdMS_TO_TICKS(ROOM_TIMEOUT_MS));
            int light = (room_mode[r] == 1) ? 1 : (room_mode[r] == 2) ? 0 : room_occupied[r];
            printf("Room %d: %s | %s | light %s (%s) | %s\n", r + 1, online ? "ONLINE" : "OFFLINE_ALERT",
                   room_occupied[r] ? "OCCUPIED" : "VACANT", light ? "ON" : "OFF", mode_names[room_mode[r]], room_stats[r]);
        }
        xSemaphoreGive(xStateMutex);
    }
    else if (strcmp(cmd, "dump") == 0) {
        printf("\n--- Data Block Log Partition Output ---\n");
        if (!sd_ready) {
            printf("No SD card.\n");
        } else {
            xSemaphoreTake(xSdMutex, portMAX_DELAY);
            FILE *f = fopen("/sdcard/access.csv", "r");
            if (f != NULL) {
                char text[128];
                while (fgets(text, sizeof(text), f) != NULL) printf("%s", text);
                fclose(f);
            } else {
                printf("Log file is empty.\n");
            }
            xSemaphoreGive(xSdMutex);
        }
    }
    else if (strncmp(cmd, "login ", 6) == 0) {
        do_login(cmd + 6);
    }
    else if (strcmp(cmd, "logout") == 0) {
        is_logged_in = false;
        printf("Logged out.\n");
    }
    else if (strncmp(cmd, "setschedule ", 12) == 0) {
        if (!require_admin()) return;
        if (sscanf(cmd + 12, "%d %d %d %d", &a, &b, &c, &d) == 4 &&
            a >= 0 && a < 24 && c >= 0 && c < 24 && b >= 0 && b < 60 && d >= 0 && d < 60) {
            xSemaphoreTake(xStateMutex, portMAX_DELAY);
            start_total = a * 60 + b;
            end_total = c * 60 + d;
            xSemaphoreGive(xStateMutex);
            printf("System parameters updated: Access window %02d:%02d - %02d:%02d\n", a, b, c, d);
            append_to_sd_card("ADMIN,setschedule,%02d:%02d-%02d:%02d", a, b, c, d);
        } else {
            printf("Usage: setschedule <h1> <m1> <h2> <m2>\n");
        }
    }
    else if (strncmp(cmd, "settime ", 8) == 0) {
        if (!require_admin()) return;
        if (sscanf(cmd + 8, "%d:%d", &a, &b) == 2 && a >= 0 && a < 24 && b >= 0 && b < 60) {
            xSemaphoreTake(xStateMutex, portMAX_DELAY);
            sys_hour = a;
            sys_min = b;
            sys_sec = 0;
            xSemaphoreGive(xStateMutex);
            printf("System base time synced to: %02d:%02d\n", a, b);
            append_to_sd_card("ADMIN,settime,%02d:%02d", a, b);
        } else {
            printf("Usage: settime <HH:MM>\n");
        }
    }
    else if (strncmp(cmd, "emergency ", 10) == 0) {
        if (!require_admin()) return;
        emergency_exit = (strcmp(cmd + 10, "on") == 0);
        printf("Emergency exit %s\n", emergency_exit ? "ON" : "OFF");
        append_to_sd_card("ADMIN,emergency,%s", emergency_exit ? "on" : "off");
    }
    else if (strncmp(cmd, "light ", 6) == 0) {
        if (!require_admin()) return;
        int room = 0;
        char mode_text[8] = "";
        sscanf(cmd + 6, "%d %7s", &room, mode_text);
        int mode = -1;
        if (strcmp(mode_text, "auto") == 0) mode = 0;
        if (strcmp(mode_text, "on") == 0)   mode = 1;
        if (strcmp(mode_text, "off") == 0)  mode = 2;
        if (room < 1 || room > 2 || mode < 0) {
            printf("Usage: light <1|2> <on|off|auto>\n");
        } else if (!mqtt_connected) {
            printf("MQTT not connected.\n");
        } else {
            room_mode[room - 1] = mode;
            send_room_command(room - 1, mode == 0 ? "AUTO" : (mode == 1 ? "ON" : "OFF"));
            append_to_sd_card("ADMIN,light,room%d=%s", room, mode_names[mode]);
        }
    }
    else if (strncmp(cmd, "adduser ", 8) == 0) {
        if (!require_admin()) return;
        char uid[16] = "", name[12] = "";
        int role = 0;
        if (sscanf(cmd + 8, "%15s %11s %d", uid, name, &role) == 3 && role >= 0 && role <= 2) {
            xSemaphoreTake(xStateMutex, portMAX_DELAY);
            bool ok = add_user(uid, name, role);
            xSemaphoreGive(xStateMutex);
            printf(ok ? "User added.\n" : "Could not add (duplicate or list full).\n");
            if (ok) append_to_sd_card("ADMIN,adduser,%s,%s", uid, name);
        } else {
            printf("Usage: adduser <UID> <name> <role 0-2>\n");
        }
    }
    else {
        printf("Unknown command. Type 'help'.\n");
    }
}

void serial_rx_task(void *pvParameters) {
    char input_line[64];
    int char_idx = 0;
    print_help();
    printf("> ");
    while (1) {
                uint8_t ch;
        int c = (uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(20)) > 0) ? (int)ch : EOF;
                if (c != EOF) {
            if (c == '\n' || c == '\r') {
                printf("\n");
                input_line[char_idx] = '\0';
                if (char_idx > 0) {
                    parse_serial_command(input_line);
                }
                printf("> ");
                char_idx = 0;
            } else if (c == 8 || c == 127) {          // backspace
                if (char_idx > 0) {
                    char_idx--;
                    printf("\b \b");
                }
            } else if (char_idx < 63) {
                input_line[char_idx++] = (char)c;
                putchar(c);                           // show what you type
            }
            fflush(stdout);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void) {
    // NVS is needed by the WiFi driver
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    xStateMutex = xSemaphoreCreateMutex();
    xSdMutex = xSemaphoreCreateMutex();

    init_gpio();
    init_sd_card();

    // Registered tags (only these are accepted)
    add_user("1111", "Alice", 0);
    add_user("2222", "Bob", 0);
    add_user("3333", "Carol", 1);
    add_user("4444", "Dan", 2);

    // Local functions work even if the network is down
    xTaskCreate(clock_task, "clock_task", 2048, NULL, 6, NULL);
    xTaskCreate(monitor_task, "monitor_task", 6144, NULL, 4, NULL);
    xTaskCreate(serial_rx_task, "serial_rx", 6144, NULL, 3, NULL);
    append_to_sd_card("SYSTEM,started");

    if (wifi_start()) {
        esp_mqtt_client_config_t mqtt_cfg = {
            .broker.address.uri = MQTT_BROKER_URI,
            .credentials.client_id = "cocis_group2026_entrance"
        };
        mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
        esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        esp_mqtt_client_start(mqtt_client);
    } else {
        printf("WiFi not connected: running without room links.\n");
    }
}
