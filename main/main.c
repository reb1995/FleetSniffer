#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "esp_task_wdt.h"

// GPS
#include "driver/uart.h"
#define UART_PORT UART_NUM_1
#define BAUD_RATE 9600

// NimBLE
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

#define PRINT_INTERVAL_MS 5000
#define DEVICE_TIMEOUT_MS 30000

#ifdef CONFIG_IDF_TARGET_ESP32C5
    #define BUZZER_PIN 9 // D9
    #define LED_PIN 27 // built in
    #define TX_PIN 11 // D6
    #define RX_PIN 12 // D7
#endif

#ifdef CONFIG_IDF_TARGET_ESP32C6
    #define BUZZER_PIN 20 // D9
    #define LED_PIN 15 //built in
    #define TX_PIN 16 // D6
    #define RX_PIN 17 // D7
    #define RF_SW_PWR_PIN 3 // Power for RF Switch
    #define RF_ANT_SEL_PIN 14 // External Antenna Select
#endif

#ifdef CONFIG_IDF_TARGET_ESP32S3
    #define BUZZER_PIN 8 // D9
    #define LED_PIN 21 // built in
    #define TX_PIN 43 // D6
    #define RX_PIN 44 // D7
#endif

#define MAX_DEVICES 256
#define MEM_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

// SPIFFS
#include "esp_spiffs.h"
#include <sys/time.h>
#include <sys/stat.h>

// --- ALERT TARGETS ---
typedef struct {
    uint8_t oui[3];
    const char *description;
} target_oui_t;

static DRAM_ATTR const char *target_ble_names[] = {};

static DRAM_ATTR const char *target_ssids[] = {
//    "", // Test SSID
    "EMS", // Generic EMS
    "Fire", // Generic Fire
    "Police", // Generic Police
    "Axon" // Generic Axon
};

static DRAM_ATTR const target_oui_t target_ouis[] = {
    { {0x02, 0x00, 0x00}, "Test OUI" }, // Private LAA. Should never actually see this OUI
    { {0x00, 0x58, 0x28}, "AXON" }, //
    { {0x00, 0xC0, 0xD4}, "AXON" }, //
    { {0x84, 0x70, 0x03}, "AXON" }, //
    { {0x00, 0x11, 0x6E}, "Peplink" }, //
    { {0x10, 0x56, 0xCA}, "Peplink" }, //
    { {0xd4, 0x13, 0xF7}, "Peplink" }, //
    { {0x28, 0xEA, 0x5B}, "Samsara" }, //
    { {0xC0, 0xD3, 0x91}, "Samsara" }, // Only /28 so false positives.
//    { {0xFC, 0xDB, 0x21}, "Samsara" }, // Samsara Networks INC // Seems to be a lot of FPs for fleet vehicles
    { {0x00, 0x1C, 0x04}, "Airgain" }, //
    { {0xB8, 0x4C, 0x87}, "Airgain" }, //
    { {0x00, 0x40, 0x02}, "Perle Systems" }, //
    { {0x00, 0x14, 0x3E}, "AirLink" }, //
    { {0x06, 0x14, 0x3E}, "AirLink Virtual" }, //
    { {0x0A, 0x14, 0x3E}, "AirLink Virtual" }, //
    { {0x28, 0xA3, 0x31}, "Sierra Wireless" }, //
    { {0x26, 0xA3, 0x31}, "Sierra Wireless Virtual" }, //
    { {0x2A, 0xA3, 0x31}, "Sierra Wireless Virtual" }, //
    { {0x00, 0x30, 0x44}, "CradlePoint" }, //
    { {0x06, 0x30, 0x44}, "CradlePoint Virtual" }, //
    { {0x00, 0xE0, 0x1C}, "CradlePoint" }, //
    { {0xE8, 0x4E, 0x06}, "EDUP" } // Cop dashcams
};

// Mostly SSIDs but could also be bluetooth names. Common false positives added.
static DRAM_ATTR const char *ignore_names[] = {
    "systems", // FP protection. Stops common EMS false positive
    "Public WiFi", // FP.
    "Public-WiFi", // FP.
    "Free Wifi", // FP.
    "LFwireless" // FP. Life Fitness default SSID.
    "Bus", // FP.
    "H&R Block", // FP.
    "Penske", // FP.
    "Tesla", // FP.
    "Drivewyze" // P.
};

const int num_target_ssids = sizeof(target_ssids) / sizeof(target_ssids[0]);
const int num_target_ble_names = sizeof(target_ble_names) / sizeof(target_ble_names[0]);
const int num_target_ouis = sizeof(target_ouis) / sizeof(target_ouis[0]);
const int num_ignore_names = sizeof(ignore_names) / sizeof(ignore_names[0]);

// ---------------------

static TaskHandle_t alert_task_handle = NULL;

typedef enum { PROTO_WIFI, PROTO_BLE } protocol_t;

typedef struct {
    uint8_t mac[6];
    protocol_t protocol;
    uint32_t packet_count;
    uint32_t probe_req_count;
    uint32_t probe_resp_count;
    uint32_t beacon_count;
    int8_t strongest_rssi;
    bool active;
    char name_ssid[33];
    uint32_t last_seen_timestamp;
    bool is_target;
    bool logged;
} device_stat_t;

// Force these arrays into internal Data RAM (DRAM)
static DRAM_ATTR device_stat_t wifi_devices_static[MAX_DEVICES];
static DRAM_ATTR device_stat_t ble_devices_static[MAX_DEVICES];

// Pointers that the rest of the code uses to access that static memory
static device_stat_t *wifi_devices = wifi_devices_static;
static device_stat_t *ble_devices = ble_devices_static;

static char gps_date_str[16] = {0};
static char gps_time_str[16] = {0};
static char gps_lat_str[16] = {0};
static char gps_lon_str[16] = {0};
static bool gps_valid = false;

static SemaphoreHandle_t data_mutex;

void print_text_log_to_console() {
    FILE* f = fopen("/spiffs/targets.txt", "r");
    if (f == NULL) {
        printf("--- No Target Log Found ---\n");
        return;
    }
    printf("\n--- SAVED TARGETS ---\n");
    printf("MAC Address       | Total | P-Req | P-Rsp | Beacons | RSSI | SSID \n");

    char line[128];
    while (fgets(line, sizeof(line), f) != NULL) {
        printf("%s", line);
    }
    printf("---------------------\n\n");
    fclose(f);
}

void append_target_to_file(device_stat_t *dev) {
    FILE* f = fopen("/spiffs/targets.txt", "a");
    if (f == NULL) return;
    if (gps_valid) {
        fprintf(f, "%02x:%02x:%02x:%02x:%02x:%02x | %-5lu | %-5lu | %-5lu | %-7lu | %-4d | %s | [%s %s] | %s, %s\n",
                dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3], dev->mac[4], dev->mac[5],
                dev->probe_req_count, dev->probe_resp_count, dev->beacon_count,
                dev->packet_count, dev->strongest_rssi, dev->name_ssid[0] ? dev->name_ssid : "<hidden>",
                gps_date_str, gps_time_str, gps_lat_str, gps_lon_str);
    } else {
        fprintf(f, "%02x:%02x:%02x:%02x:%02x:%02x | %-5lu | %-5lu | %-5lu | %-7lu | %-4d | %s\n",
                dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3], dev->mac[4], dev->mac[5],
                dev->probe_req_count, dev->probe_resp_count, dev->beacon_count,
                dev->packet_count, dev->strongest_rssi, dev->name_ssid[0] ? dev->name_ssid : "<hidden>");
    }
    fclose(f);
}

static uint32_t IRAM_ATTR hash_mac(const uint8_t *mac) {
    uint32_t hash = 5381;
    for (int i = 0; i < 6; i++) {
        hash = ((hash << 5) + hash) + mac[i];
    }
    return hash % MAX_DEVICES;
}

int compare_devices(const void *a, const void *b) {
    device_stat_t *devA = (device_stat_t *)a;
    device_stat_t *devB = (device_stat_t *)b;
    if (devA->is_target && !devB->is_target) return -1;
    if (!devA->is_target && devB->is_target) return 1;
    return (devB->strongest_rssi - devA->strongest_rssi);
}

static void IRAM_ATTR update_device_record(device_stat_t *list, const uint8_t *mac, protocol_t proto, int8_t rssi, uint8_t subtype, const char *name, bool is_target) {
    if (xSemaphoreTake(data_mutex, 0) == pdTRUE) {
        uint32_t start_idx = hash_mac(mac);
        uint32_t idx = start_idx;

        for (int i = 0; i < MAX_DEVICES; i++) {
            if (!list[idx].active) {
                memcpy(list[idx].mac, mac, 6);
                list[idx].protocol = proto;
                list[idx].packet_count = 1;
                list[idx].strongest_rssi = rssi;
                list[idx].active = true;
                list[idx].is_target = is_target;

                if (proto == PROTO_WIFI) {
                    if (subtype == 4) list[idx].probe_req_count = 1;
                    else if (subtype == 5) list[idx].probe_resp_count = 1;
                    else if (subtype == 8) list[idx].beacon_count = 1;
                }

                if (name && name[0] != '\0') {
                    strlcpy(list[idx].name_ssid, name, sizeof(list[idx].name_ssid));
                }
                list[idx].last_seen_timestamp = xTaskGetTickCount();
                break;
            } else if (memcmp(list[idx].mac, mac, 6) == 0) {
                list[idx].packet_count++;
                if (is_target) list[idx].is_target = true;

                if (proto == PROTO_WIFI) {
                    if (subtype == 4) list[idx].probe_req_count++;
                    else if (subtype == 5) list[idx].probe_resp_count++;
                    else if (subtype == 8) list[idx].beacon_count++;
                }

                if (rssi > list[idx].strongest_rssi) {
                    list[idx].strongest_rssi = rssi;
                }
                if (name && name[0] != '\0') {
                    strlcpy(list[idx].name_ssid, name, sizeof(list[idx].name_ssid));
                }
                list[idx].last_seen_timestamp = xTaskGetTickCount();
                break;
            }
            idx = (idx + 1) % MAX_DEVICES;
        }
        xSemaphoreGive(data_mutex);
    }
}

void alert_task(void *pvParameters) {
    gpio_set_direction(BUZZER_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(BUZZER_PIN, 0);
    gpio_set_level(LED_PIN, 1);

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        gpio_set_level(BUZZER_PIN, 1);
        gpio_set_level(LED_PIN, 0);

        xSemaphoreTake(data_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_DEVICES; i++) {
            if (wifi_devices[i].active && wifi_devices[i].is_target && !wifi_devices[i].logged) {
                append_target_to_file(&wifi_devices[i]);
                wifi_devices[i].logged = true;
            }
            if (ble_devices[i].active && ble_devices[i].is_target && !ble_devices[i].logged) {
                append_target_to_file(&ble_devices[i]);
                ble_devices[i].logged = true;
            }
        }
        xSemaphoreGive(data_mutex);

        vTaskDelay(pdMS_TO_TICKS(150));
        gpio_set_level(BUZZER_PIN, 0);
        gpio_set_level(LED_PIN, 1);
    }
}

/* ========================================================================== *
 * WI-FI SNIFFER                                                              *
 * ========================================================================== */

static void IRAM_ATTR  wifi_sniffer_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    if (pkt->rx_ctrl.sig_len < 24) return;

    uint8_t *payload = pkt->payload;
    uint8_t fc = payload[0];
    uint8_t f_subtype = (fc >> 4) & 0xF;
    if (f_subtype != 4 && f_subtype != 5 && f_subtype != 8) return;

    uint8_t *mac = &payload[10];
    char temp_ssid[33] = {0};

    if (pkt->rx_ctrl.sig_len > 36) {
        uint8_t *tags = payload + 24;
        if (f_subtype == 8 || f_subtype == 5) tags = payload + 36;
        int max_len = pkt->rx_ctrl.sig_len - (tags - payload);
        int i = 0;
        while (i < max_len - 1) {
            uint8_t tid = tags[i], tlen = tags[i+1];
            if (i + 2 + tlen > max_len) break;
            if (tid == 0 && tlen > 0 && tlen <= 32) {
                memcpy(temp_ssid, &tags[i+2], tlen);
                temp_ssid[tlen] = '\0';
                break;
            }
            i += 2 + tlen;
        }
    }

    bool trigger = false;
    for (int i = 0; i < num_target_ouis; i++) {
        if (memcmp(mac, target_ouis[i].oui, 3) == 0) trigger = true;
    }
    for (int i = 0; i < num_target_ssids; i++) {
        if (temp_ssid[0] != '\0' && strcasestr(temp_ssid, target_ssids[i]) != NULL) trigger = true;
    }
    if (trigger && temp_ssid[0] != '\0') {
        for (int i = 0; i < num_ignore_names; i++) {
            if (strcasestr(temp_ssid, ignore_names[i]) != NULL) { trigger = false; break; }
        }
    }

    update_device_record(wifi_devices, mac, PROTO_WIFI, pkt->rx_ctrl.rssi, f_subtype, temp_ssid, trigger);

    if (trigger) {
        if (alert_task_handle) xTaskNotifyGive(alert_task_handle);
    }
}

#ifdef CONFIG_IDF_TARGET_ESP32C5
static const uint8_t channels_5g[] = {36, 40, 44, 48, 149, 153, 157, 161};
#endif

void wifi_channel_hopper(void *pvParameters) {
    int channel_2g = 1;

    while (1) {
#ifdef CONFIG_IDF_TARGET_ESP32C5
        esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
#endif
        for(int i = 0; i < 11; i++) {
            esp_wifi_set_channel(channel_2g, WIFI_SECOND_CHAN_NONE);
            channel_2g = (channel_2g % 11) + 1;
            vTaskDelay(pdMS_TO_TICKS(150));
        }

#ifdef CONFIG_IDF_TARGET_ESP32C5
        esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY);
        int num_5g_channels = sizeof(channels_5g) / sizeof(channels_5g[0]);
        for(int i = 0; i < num_5g_channels; i++) {
            esp_wifi_set_channel(channels_5g[i], WIFI_SECOND_CHAN_NONE);
            vTaskDelay(pdMS_TO_TICKS(150));
        }
#endif
    }
}

/* ========================================================================== *
 * BLE SCANNER                                                                *
 * ========================================================================== */

static int IRAM_ATTR ble_gap_event_cb(struct ble_gap_event *event, void *arg) {
    if (event->type == BLE_GAP_EVENT_DISC) {
        struct ble_gap_disc_desc *desc = &event->disc;
        char dev_name[33] = {0};
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, desc->data, desc->length_data) == 0) {
            if (fields.name != NULL && fields.name_len > 0) {
                int len = fields.name_len < 32 ? fields.name_len : 32;
                memcpy(dev_name, fields.name, len);
                dev_name[len] = '\0';
            }
        }
        uint8_t mac[6];
        for(int i=0; i<6; i++) mac[i] = desc->addr.val[5-i];

        bool trigger = false;
        for (int i = 0; i < num_target_ouis; i++) {
            if (memcmp(mac, target_ouis[i].oui, 3) == 0) trigger = true;
        }
        for (int i = 0; i < num_target_ble_names; i++) {
            if (dev_name[0] != '\0' && strcasestr(dev_name, target_ble_names[i]) != NULL) trigger = true;
        }

        if (trigger && dev_name[0] != '\0') {
            for (int i = 0; i < num_ignore_names; i++) {
                if (strcasestr(dev_name, ignore_names[i]) != NULL) {
                    trigger = false;
                    break;
                }
            }
        }

        update_device_record(ble_devices, mac, PROTO_BLE, desc->rssi, 0, dev_name, trigger);

        if (trigger && alert_task_handle != NULL) {
            xTaskNotifyGive(alert_task_handle);
        }
    }
    return 0;
}

static void ble_on_sync(void) {
    struct ble_gap_disc_params disc_params = {0};
    disc_params.filter_duplicates = 0;
    disc_params.passive = 0;
    disc_params.itvl = 160;
    disc_params.window = 80;

    ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &disc_params, ble_gap_event_cb, NULL);
}

void ble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ========================================================================== *
 * CONSOLE OUTPUT                                                             *
 * ========================================================================== */

void monitor_print_task(void *pvParameters) {
    device_stat_t *sorted_copy = heap_caps_malloc(MAX_DEVICES * sizeof(device_stat_t), MEM_CAPS);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(PRINT_INTERVAL_MS));
        int wifi_count = 0, ble_count = 0;
        uint32_t current_time = xTaskGetTickCount();

        if (xSemaphoreTake(data_mutex, portMAX_DELAY)) {
            for (int i = 0; i < MAX_DEVICES; i++) {
                if (wifi_devices[i].active) {
                    if ((current_time - wifi_devices[i].last_seen_timestamp) * portTICK_PERIOD_MS > DEVICE_TIMEOUT_MS) {
                        memset(&wifi_devices[i], 0, sizeof(device_stat_t));
                    } else if (wifi_devices[i].packet_count > 0) {
                        memcpy(&sorted_copy[wifi_count++], &wifi_devices[i], sizeof(device_stat_t));
                    }
                }
            }

            qsort(sorted_copy, wifi_count, sizeof(device_stat_t), compare_devices);
            printf("\033[2J\033[H");
            if (gps_valid) {
                printf("Date: %s | Time (UTC): %s | Location: %s, %s\n", gps_date_str, gps_time_str, gps_lat_str, gps_lon_str);
            }
            printf("--- WI-FI DEVICES (%d) ---\n", wifi_count);
            printf(" MAC Address       | Total | P-Req | P-Rsp | Beacons | RSSI | SSID \n");
            for (int i = 0; i < (wifi_count < 30 ? wifi_count : 30); i++) {
                printf(" %02x:%02x:%02x:%02x:%02x:%02x | %-5lu | %-5lu | %-5lu | %-7lu | %-4d | %s\n",
                       sorted_copy[i].mac[0], sorted_copy[i].mac[1], sorted_copy[i].mac[2],
                       sorted_copy[i].mac[3], sorted_copy[i].mac[4], sorted_copy[i].mac[5],
                       sorted_copy[i].packet_count, sorted_copy[i].probe_req_count,
                       sorted_copy[i].probe_resp_count, sorted_copy[i].beacon_count,
                       sorted_copy[i].strongest_rssi, sorted_copy[i].name_ssid);
            }

            for (int i = 0; i < MAX_DEVICES; i++) {
                if (ble_devices[i].active) {
                    if ((current_time - ble_devices[i].last_seen_timestamp) * portTICK_PERIOD_MS > DEVICE_TIMEOUT_MS) {
                        memset(&ble_devices[i], 0, sizeof(device_stat_t));
                    } else if (ble_devices[i].packet_count > 0) {
                        memcpy(&sorted_copy[ble_count++], &ble_devices[i], sizeof(device_stat_t));
                    }
                }
            }

            qsort(sorted_copy, ble_count, sizeof(device_stat_t), compare_devices);
            printf("\n--- BLE DEVICES (%d) ---\n", ble_count);
            printf(" MAC Address       | Packets | RSSI | Name \n");
            for (int i = 0; i < (ble_count < 7 ? ble_count : 7); i++) {
                printf(" %02x:%02x:%02x:%02x:%02x:%02x | %-7lu | %-4d | %s\n",
                       sorted_copy[i].mac[0], sorted_copy[i].mac[1], sorted_copy[i].mac[2],
                       sorted_copy[i].mac[3], sorted_copy[i].mac[4], sorted_copy[i].mac[5],
                       sorted_copy[i].packet_count, sorted_copy[i].strongest_rssi,
                       sorted_copy[i].name_ssid);
            }

            xSemaphoreGive(data_mutex);
        }
    }
}

// GPS Task
static void gps_task(void *arg) {
    uart_config_t uart_config = {
        .baud_rate = BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_PORT, 1024, 0, 0, NULL, 0);
    uart_param_config(UART_PORT, &uart_config);
    uart_set_pin(UART_PORT, TX_PIN, RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uint8_t data[128];
    char line[128];
    int line_len = 0;
    while (1) {
        int rxBytes = uart_read_bytes(UART_PORT, data, sizeof(data) - 1, pdMS_TO_TICKS(100));
        for (int i = 0; i < rxBytes; i++) {
            if (data[i] == '\n') {
                line[line_len] = '\0';
                if (strncmp(line, "$GNRMC", 6) == 0 || strncmp(line, "$GPRMC", 6) == 0) {
                    char *tokens[15];
                    int t_cnt = 0;
                    char *ptr = line;
                    char *token = ptr;
                    while (*ptr) {
                        if (*ptr == ',') {
                            *ptr = '\0';
                            tokens[t_cnt++] = token;
                            token = ptr + 1;
                            if (t_cnt >= 15) break;
                        }
                        ptr++;
                    }
                    if (t_cnt < 15) tokens[t_cnt++] = token;
                    if (t_cnt > 6 && strcmp(tokens[2], "A") == 0) {
                        xSemaphoreTake(data_mutex, portMAX_DELAY);
                        gps_valid = true;
                        if (t_cnt > 9 && strlen(tokens[9]) >= 6) {
                            snprintf(gps_date_str, sizeof(gps_date_str), "20%c%c-%c%c-%c%c",
                                     tokens[9][4], tokens[9][5],
                                     tokens[9][2], tokens[9][3],
                                     tokens[9][0], tokens[9][1]);
                        }
                        if (strlen(tokens[1]) >= 6) {
                            snprintf(gps_time_str, sizeof(gps_time_str), "%c%c:%c%c:%c%c",
                                     tokens[1][0], tokens[1][1],
                                     tokens[1][2], tokens[1][3],
                                     tokens[1][4], tokens[1][5]);
                        } else {
                            strncpy(gps_time_str, tokens[1], sizeof(gps_time_str)-1);
                        }                        double lat_deg, lat_min, lon_deg, lon_min;
                        sscanf(tokens[3], "%2lf%lf", &lat_deg, &lat_min);
                        sscanf(tokens[5], "%3lf%lf", &lon_deg, &lon_min);
                        double lat = lat_deg + (lat_min / 60.0);
                        double lon = lon_deg + (lon_min / 60.0);
                        if (tokens[4][0] == 'S') lat = -lat;
                        if (tokens[6][0] == 'W') lon = -lon;
                        snprintf(gps_lat_str, sizeof(gps_lat_str), "%.5f", lat);
                        snprintf(gps_lon_str, sizeof(gps_lon_str), "%.5f", lon);
                        xSemaphoreGive(data_mutex);
                    } else {
                        xSemaphoreTake(data_mutex, portMAX_DELAY);
                        gps_valid = false;
                        xSemaphoreGive(data_mutex);
                    }
                }
                line_len = 0;
            } else if (data[i] != '\r' && line_len < sizeof(line) - 1) {
                line[line_len++] = (char)data[i];
            }
        }
    }
}

/* ========================================================================== *
 * ENTRY                                                                      *
 * ========================================================================== */

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());

    esp_vfs_spiffs_conf_t conf = { .base_path = "/spiffs", .partition_label = NULL, .max_files = 5, .format_if_mount_failed = true };
    esp_vfs_spiffs_register(&conf);

    print_text_log_to_console();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    xTaskCreate(alert_task, "alert_task", 2048, NULL, 10, &alert_task_handle);

    data_mutex = xSemaphoreCreateMutex();

#ifdef CONFIG_IDF_TARGET_ESP32C6
    // Seeed XIAO ESP32-C6: Route RF to the external U.FL connector
    gpio_reset_pin(RF_SW_PWR_PIN);
    gpio_set_direction(RF_SW_PWR_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(RF_SW_PWR_PIN, 0); // LOW = Power ON the RF switch

    // Allow RF switch voltage to stabilize
    vTaskDelay(pdMS_TO_TICKS(100));

    gpio_reset_pin(RF_ANT_SEL_PIN);
    gpio_set_direction(RF_ANT_SEL_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(RF_ANT_SEL_PIN, 1); // HIGH = Select External Antenna
#endif

    esp_log_level_set("wifi", ESP_LOG_WARN);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(ble_host_task);

    xTaskCreate(gps_task, "gps_task", 3072, NULL, 5, NULL);
    xTaskCreate(wifi_channel_hopper, "wifi_hop", 1536, NULL, 5, NULL);
    xTaskCreate(monitor_print_task, "printer", 3072, NULL, 4, NULL);

    // Beep on boot to know it booted fine
    xTaskNotifyGive(alert_task_handle);
}
