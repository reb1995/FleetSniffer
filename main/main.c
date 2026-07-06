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
#include "soc/soc_caps.h"

// GPS
#include "driver/uart.h"
#define UART_PORT UART_NUM_1
#define BAUD_RATE 9600

// NimBLE
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

// SPIFFS
#include "esp_spiffs.h"
#include <sys/time.h>
#include <sys/stat.h>

// Screen
#include "driver/i2c.h"

//#define PRINT_INTERVAL_MS 5000
#define PRINT_INTERVAL_START_MS 2000
#define PRINT_INTERVAL_END_MS 2000
#define DEVICE_TIMEOUT_MS 10000

#define I2C_MASTER_NUM  0
#define I2C_MASTER_FREQ_HZ 400000
#define OLED_ADDR 0x3C

static bool has_screen = false;
static SemaphoreHandle_t oled_mutex;

static char oled_line1[38] = "WAITING FOR";
static char oled_line2[38] = "TARGET";
static char oled_line3[38] = "";

#ifdef CONFIG_IDF_TARGET_ESP32C5
    #define BUZZER_PIN 9 // D9
    #define LED_PIN 27 // built in
    #define TX_PIN 11 // D6
    #define RX_PIN 12 // D7
    #define I2C_MASTER_SDA_IO 23
    #define I2C_MASTER_SCL_IO 24
#endif

#ifdef CONFIG_IDF_TARGET_ESP32C6
    #define BUZZER_PIN 20 // D9
    #define LED_PIN 15 //built in
    #define TX_PIN 16 // D6
    #define RX_PIN 17 // D7
    #define RF_SW_PWR_PIN 3    // Power for RF Switch
    #define RF_ANT_SEL_PIN 14  // External Antenna Select
    #define I2C_MASTER_SDA_IO 22
    #define I2C_MASTER_SCL_IO 23
#endif

#ifdef CONFIG_IDF_TARGET_ESP32S3
    #define BUZZER_PIN 1 // D9
    #define LED_PIN 21 // built in
    #define TX_PIN 43 // D6
    #define RX_PIN 44 // D7
    #define I2C_MASTER_SDA_IO 5
    #define I2C_MASTER_SCL_IO 6
#endif

#define MAX_DEVICES 512

#define MEM_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

// --- ALERT TARGETS ---
typedef struct {
    uint8_t oui[3];
    const char *description;
} target_oui_t;

static DRAM_ATTR const char *target_ble_names[] = {
    "AXON"
};

static DRAM_ATTR const char *target_ssids[] = {
//    ".", // Test SSID
    "Fire", // Generic
    "EMS", // Generic
    "Police", // Generic
    "Axon" // Generic
};

static DRAM_ATTR const target_oui_t target_ouis[] = {
//    { {0x02, 0x00, 0x00}, "Test OUI" }, // Private LAA. Should never actually see this OUI. But you do, apparently.
    { {0x00, 0x58, 0x28}, "AXON" }, // Axon.
    { {0x00, 0xC0, 0xD4}, "AXON" }, // Axon
    { {0x84, 0x70, 0x03}, "AXON" }, // Axon
    { {0x00, 0x25, 0xDF}, "AXON Enterprise" }, // Axon Enterprise. Probably only cops.
    { {0x00, 0x11, 0x6E}, "Peplink" }, // Peplink
    { {0x10, 0x56, 0xCA}, "Peplink" }, // Peplink
    { {0xd4, 0x13, 0xF7}, "Peplink" }, // Peplink
    { {0x28, 0xEA, 0x5B}, "Samsara" }, // Samsara Networks INC
    { {0xC0, 0xD3, 0x91}, "Samsara" }, // Samsara Networks INC. Only /28 so false positives.
    { {0x00, 0x1C, 0x04}, "Airgain" }, // Airgain INC
    { {0xB8, 0x4C, 0x87}, "Airgain" }, // Airgain INC
    { {0x00, 0x40, 0x02}, "Perle Systems" }, // Perle Systems
    { {0x00, 0x14, 0x3E}, "AirLink" }, //
    { {0x28, 0xA3, 0x31}, "Sierra Wireless" }, //
    { {0x00, 0x30, 0x44}, "CradlePoint" }, //
    { {0x00, 0xE0, 0x1C}, "CradlePoint" }, //
    { {0xE8, 0x4E, 0x06}, "EDUP" }, // Dashcams
    { {0x34, 0x8A, 0x12}, "HPE" },
    { {0xCC, 0x78, 0xAB}, "Zebra Printer" }, // Mobile ticket printers
    { {0xAC, 0x4D, 0x16}, "Zebra Printer" }, // Mobile ticket printers
    { {0xC4, 0xD3, 0x6A}, "Zebra Printer" } // Mobile ticket printers
};

// Mostly SSIDs but could also be bluetooth names.
static DRAM_ATTR const char *ignore_names[] = {
    "problems", // FP protection. Stops common EMS false positive
    "systems", // FP protection. Stops common EMS false positive
    "not in use", // Mostly Raleigh Buses
    "Public WiFi", // FP. Public WiFi probably isn't a hit...
    "Public-WiFi", // FP. Public WiFi probably isn't a hit...
    "Bus", // FP.
    "CTFCU", // Local business: "Carolinas Telco Federal Credit Union"
    "ALLSystemsGo", // FP.
    "RAI-SIGNAGE", // FP. CradlePoint
    "IKE Free Wifi", // FP. Raleigh Interactive Kiosks. Cradlepoint
    "AMRED", // FP. American Red Cross. Cradlepoint
    "LFwireless" // FP. Life Fitness default SSID. EDUP.
    "WHUIS", // FP. Waffle House?
    "ecoATM", // FP.
    "PrePass", // FP.
    "SaleemsFamily", // FP.
    "SprinterWiFi", // FP.
    "IncidentClear", // FP.
    "H&R Block", // FP.
    "Health Team", // FP.
    "Tunnel Hill", // FP.
    "PureTech", // FP.
    "NiFleet", // FP.
    "Penske", // FP.
    "Tesla", // FP.
    "Drivewyze", // FP.
    "CIS-RX", // FP.
    "GoEnergy", // FP.
    "AVIT", // FP.
    "JimmyJohns", // FP.
    "Transit", // FP.
    "Rivian" // FP.
};

const int num_target_ssids = sizeof(target_ssids) / sizeof(target_ssids[0]);
const int num_target_ble_names = sizeof(target_ble_names) / sizeof(target_ble_names[0]);
const int num_target_ouis = sizeof(target_ouis) / sizeof(target_ouis[0]);
const int num_ignore_names = sizeof(ignore_names) / sizeof(ignore_names[0]);

static char gps_date_str[16] = {0};
static char gps_time_str[16] = {0};
static char gps_lat_str[16] = {0};
static char gps_lon_str[16] = {0};
static int gps_satellites_tracked = 0;
static int temp_sats_in_view = 0;
static bool gps_valid = false;
static volatile bool gps_hardware_detected = false;

static SemaphoreHandle_t data_mutex;

// ---------------------
// OLED Functions
static const uint8_t font5x8[96][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x08,0x2A,0x1C,0x2A,0x08},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},{0x41,0x22,0x14,0x08,0x00},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x01,0x01},{0x3E,0x41,0x41,0x51,0x32},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x04,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x7F,0x20,0x18,0x20,0x7F},
    {0x63,0x14,0x08,0x14,0x63},{0x03,0x04,0x78,0x04,0x03},{0x61,0x51,0x49,0x45,0x43},{0x00,0x00,0x7F,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20},{0x41,0x41,0x7F,0x00,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x08,0x14,0x54,0x54,0x3C},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},{0x00,0x7F,0x10,0x28,0x44},
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x08,0x04,0x08,0x10,0x08},{0x00,0x00,0x00,0x00,0x00}
};

static void oled_cmd(uint8_t cmd) {
    if(!has_screen) return;
    uint8_t data[2] = {0x00, cmd};
    i2c_master_write_to_device(I2C_MASTER_NUM, OLED_ADDR, data, 2, pdMS_TO_TICKS(100));
}

static void ssd1306_clear(void) {
    if(!has_screen) return;
    uint8_t data[129];
    data[0] = 0x40;
    memset(&data[1], 0x00, 128);
    for (uint8_t page = 0; page < 8; page++) {
        oled_cmd(0xB0 + page); oled_cmd(0x00); oled_cmd(0x10);
        i2c_master_write_to_device(I2C_MASTER_NUM, OLED_ADDR, data, 129, pdMS_TO_TICKS(100));
    }
}

static void ssd1306_print_line_2x(uint8_t page, const char* str, bool invert) {
    if(!has_screen || !str) return;

    uint8_t data_top[129];
    uint8_t data_bot[129];
    data_top[0] = 0x40;
    data_bot[0] = 0x40;
    memset(&data_top[1], invert ? 0xFF : 0x00, 128);
    memset(&data_bot[1], invert ? 0xFF : 0x00, 128);

    int len = strlen(str);
    if (len > 21) len = 21;

    int start_x = (128 - (len * 6)) / 2; // Center alignment
    int idx = 1 + start_x;

    for (int i = 0; i < len; i++) {
        char c = str[i];
        if (c < 32 || c > 127) c = '?';
        for (int col = 0; col < 5; col++) {
            uint8_t b = font5x8[c - 32][col];
            if (invert) b = ~b;

            // Stretch bits vertically
            uint8_t top = 0, bot = 0;
            for (int bit = 0; bit < 4; bit++) {
                if (b & (1 << bit)) top |= (3 << (bit * 2));
                if (b & (1 << (bit + 4))) bot |= (3 << (bit * 2));
            }
            data_top[idx] = top;
            data_bot[idx] = bot;
            idx++;
        }
        data_top[idx] = invert ? 0xFF : 0x00;
        data_bot[idx] = invert ? 0xFF : 0x00;
        idx++;
    }

    uint8_t cmds_top[4] = {0x00, 0xB0 + page, 0x00, 0x10};
    i2c_master_write_to_device(I2C_MASTER_NUM, OLED_ADDR, cmds_top, 4, pdMS_TO_TICKS(100));
    i2c_master_write_to_device(I2C_MASTER_NUM, OLED_ADDR, data_top, 129, pdMS_TO_TICKS(100));

    uint8_t cmds_bot[4] = {0x00, 0xB0 + page + 1, 0x00, 0x10};
    i2c_master_write_to_device(I2C_MASTER_NUM, OLED_ADDR, cmds_bot, 4, pdMS_TO_TICKS(100));
    i2c_master_write_to_device(I2C_MASTER_NUM, OLED_ADDR, data_bot, 129, pdMS_TO_TICKS(100));
}

static void init_oled(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (OLED_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);

    if (ret == ESP_OK) {
        has_screen = true;
        uint8_t init_cmds[] = {
            0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
            0x8D, 0x14, 0x20, 0x02, 0xA1, 0xC8, 0xDA, 0x12,
            0x81, 0xFF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF
        };
        for(int i=0; i<sizeof(init_cmds); i++) oled_cmd(init_cmds[i]);
        ssd1306_clear();
    }
}

void oled_task(void *pvParameters) {
    while (1) {
        if (has_screen) {
            if (xSemaphoreTake(oled_mutex, portMAX_DELAY)) {
                ssd1306_print_line_2x(0, oled_line1, false);
                ssd1306_print_line_2x(3, oled_line2, false);
                ssd1306_print_line_2x(6, oled_line3, false);
                xSemaphoreGive(oled_mutex);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// OLED Functions
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
    uint8_t channel;
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

void print_text_log_to_console() {
    FILE* f = fopen("/spiffs/targets.txt", "r");
    if (f == NULL) {
        printf("--- No Target Log Found ---\n");
        return;
    }
    printf("\n--- SAVED TARGETS ---\n");
    printf("MAC Address       | Total | P-Req | P-Rsp | Beacons | RSSI | Chan | SSID | Vendor \n");

    char line[128];
    while (fgets(line, sizeof(line), f) != NULL) {
        printf("%s", line);
        vTaskDelay(1);
    }
    printf("---------------------\n\n");
    fclose(f);
}

void append_target_to_file(device_stat_t *dev) {
    FILE* f = fopen("/spiffs/targets.txt", "a");
    if (f == NULL) return;

    const char *vendor = "Unknown";
    if ((dev->mac[1] == 0x30 && dev->mac[2] == 0x44) || (dev->mac[1] == 0xE0 && dev->mac[2] == 0x1C)) {
        vendor = "CradlePoint vMAC";
    } else if (dev->mac[1] == 0xA3 && dev->mac[2] == 0x31) {
        vendor = "Sierra W vMAC";
    } else if (dev->mac[1] == 0x14 && dev->mac[2] == 0x3E) {
        vendor = "AirLink vMAC";
    } else {
        for(int j=0; j<num_target_ouis; j++) {
            if(memcmp(dev->mac, target_ouis[j].oui, 3) == 0) {
                vendor = target_ouis[j].description;
                break;
            }
        }
    }

    if (gps_valid) {
        fprintf(f, "%02x:%02x:%02x:%02x:%02x:%02x | %-5lu | %-5lu | %-5lu | %-7lu | %-4d | %-4u | %s | %s | [%s %s] | %s, %s\n",
                dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3], dev->mac[4], dev->mac[5],
                dev->packet_count, dev->probe_req_count, dev->probe_resp_count, dev->beacon_count,
                dev->strongest_rssi, dev->channel, dev->name_ssid[0] ? dev->name_ssid : "<hidden>", vendor,
                gps_date_str, gps_time_str, gps_lat_str, gps_lon_str);
    } else {
        fprintf(f, "%02x:%02x:%02x:%02x:%02x:%02x | %-5lu | %-5lu | %-5lu | %-7lu | %-4d | %-4u | %s | %s\n",
                dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3], dev->mac[4], dev->mac[5],
                dev->packet_count, dev->probe_req_count, dev->probe_resp_count, dev->beacon_count,
                dev->strongest_rssi, dev->channel, dev->name_ssid[0] ? dev->name_ssid : "<no name>", vendor);
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

static void IRAM_ATTR update_device_record(device_stat_t *list, const uint8_t *mac, protocol_t proto, int8_t rssi, uint8_t channel, uint8_t subtype, const char *name, bool is_target) {
    if (xSemaphoreTake(data_mutex, 0) == pdTRUE) {
        uint32_t start_idx = hash_mac(mac);
        uint32_t idx = start_idx;

        for (int i = 0; i < MAX_DEVICES; i++) {
            if (!list[idx].active) {
                memcpy(list[idx].mac, mac, 6);
                list[idx].protocol = proto;
                list[idx].packet_count = 1;
                list[idx].strongest_rssi = rssi;
                list[idx].channel = channel;
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
                if (is_target) {
                    if ((name && name[0] != '\0') || list[idx].name_ssid[0] == '\0') {
                        list[idx].is_target = true;
                    }
                }
                if (proto == PROTO_WIFI) {
                    if (subtype == 4) list[idx].probe_req_count++;
                    else if (subtype == 5) list[idx].probe_resp_count++;
                    else if (subtype == 8) list[idx].beacon_count++;
                }
                if (rssi > list[idx].strongest_rssi) {
                    list[idx].strongest_rssi = rssi;
                    list[idx].channel = channel;
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

        // GRACE PERIOD: Wait 250ms to see if a beacon arrives to kill the alert
        vTaskDelay(pdMS_TO_TICKS(250));

        device_stat_t temp_target;
        device_stat_t *display_target = NULL;
        bool needs_logging = false;
        bool trigger_buzzer = false;

        // Process WI-FI Devices
        for (int i = 0; i < MAX_DEVICES; i++) {
            if (xSemaphoreTake(data_mutex, portMAX_DELAY)) {
                // If it is a target, it sets off the alarm
                if (wifi_devices[i].active && wifi_devices[i].is_target) {
                    trigger_buzzer = true;
                    display_target = &wifi_devices[i];

                    // If it hasn't been logged yet, copy it to be saved
                    if (!wifi_devices[i].logged) {
                        memcpy(&temp_target, &wifi_devices[i], sizeof(device_stat_t));
                        wifi_devices[i].logged = true;
                        needs_logging = true;
                    }
                }
                xSemaphoreGive(data_mutex);
            }
            if (needs_logging) {
                append_target_to_file(&temp_target);
                needs_logging = false;
            }
        }

        // Process BLE Devices
        for (int i = 0; i < MAX_DEVICES; i++) {
            if (xSemaphoreTake(data_mutex, portMAX_DELAY)) {
                if (ble_devices[i].active && ble_devices[i].is_target) {
                    trigger_buzzer = true;
                    display_target = &ble_devices[i];

                    if (!ble_devices[i].logged) {
                        memcpy(&temp_target, &ble_devices[i], sizeof(device_stat_t));
                        ble_devices[i].logged = true;
                        needs_logging = true;
                    }
                }
                xSemaphoreGive(data_mutex);
            }
            if (needs_logging) {
                append_target_to_file(&temp_target);
                needs_logging = false;
            }
        }

        // Only sound the alarm if a target survived the grace period
        if (trigger_buzzer) {
                if (display_target != NULL && xSemaphoreTake(oled_mutex, portMAX_DELAY)) {
                if (display_target->protocol == PROTO_WIFI) {
                    snprintf(oled_line1, sizeof(oled_line1), "WIFI: C:%d RSSI:%d", display_target->channel, display_target->strongest_rssi);
                    snprintf(oled_line3, sizeof(oled_line3), "%s", display_target->name_ssid[0] ? display_target->name_ssid : "<HIDDEN>");
                } else {
                    snprintf(oled_line1, sizeof(oled_line1), "BLE: RSSI:%d", display_target->strongest_rssi);
                    snprintf(oled_line3, sizeof(oled_line3), "%s", display_target->name_ssid[0] ? display_target->name_ssid : "<HIDDEN>");
                }

                // Update in alert_task
                strlcpy(oled_line2, " ", sizeof(oled_line2));
                if ((display_target->mac[1] == 0x30 && display_target->mac[2] == 0x44) ||
                    (display_target->mac[1] == 0xE0 && display_target->mac[2] == 0x1C)) {
                    snprintf(oled_line2, sizeof(oled_line2), "CradlePoint vMAC");
                } else if (display_target->mac[1] == 0xA3 && display_target->mac[2] == 0x31) {
                    snprintf(oled_line2, sizeof(oled_line2), "Sierra W vMAC");
                } else if (display_target->mac[1] == 0x14 && display_target->mac[2] == 0x3E) {
                    snprintf(oled_line2, sizeof(oled_line2), "AirLink vMAC");
                } else {
                    for(int j=0; j<num_target_ouis; j++) {
                        if(memcmp(display_target->mac, target_ouis[j].oui, 3) == 0) {
                            snprintf(oled_line2, sizeof(oled_line2), "%s", target_ouis[j].description);
                            break;
                        }
                    }
                }
                xSemaphoreGive(oled_mutex);
            }

            gpio_set_level(BUZZER_PIN, 1);
            gpio_set_level(LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(150));
            gpio_set_level(BUZZER_PIN, 0);
            gpio_set_level(LED_PIN, 1);
        }
    }
}

/* ========================================================================== *
 * WI-FI SNIFFER                                                              *
 * ========================================================================== */
static void IRAM_ATTR wifi_sniffer_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    if (pkt->rx_ctrl.sig_len < 24) return;

    uint8_t *payload = pkt->payload;
    uint8_t fc = payload[0];
    uint8_t f_subtype = (fc >> 4) & 0xF;

    uint8_t *mac_dst = &payload[4];
    uint8_t *mac_src = &payload[10];
    uint8_t *mac_to_log = mac_src; // Default to logging the source

    char temp_ssid[33] = {0};
    bool trigger = false;
    uint8_t pass_subtype = 0;

    // --- MANAGEMENT FRAME PROCESSING ---
    if (type == WIFI_PKT_MGMT) {
        if (f_subtype != 4 && f_subtype != 5 && f_subtype != 8) return;
        pass_subtype = f_subtype;

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

        if ((mac_src[1] == 0x30 && mac_src[2] == 0x44 && (mac_src[0] == 0x00 || (mac_src[0] & 0x02))) || // Cradlepoint Base
            (mac_src[1] == 0xE0 && mac_src[2] == 0x1C && (mac_src[0] == 0x00 || (mac_src[0] & 0x02))) || // Cradlepoint Base 2
            (mac_src[1] == 0xA3 && mac_src[2] == 0x31 && (mac_src[0] == 0x28 || (mac_src[0] & 0x02))) || // Sierra Wireless Base
            (mac_src[1] == 0x14 && mac_src[2] == 0x3E && (mac_src[0] == 0x00 || (mac_src[0] & 0x02)))) { // Airlink Base
            trigger = true;
        } else {
            for (int i = 0; i < num_target_ouis; i++) {
                if (memcmp(mac_src, target_ouis[i].oui, 3) == 0) {
                    trigger = true;
                    break;
                }
            }
        }

        for (int i = 0; i < num_target_ssids; i++) {
            if (temp_ssid[0] != '\0' && strcasestr(temp_ssid, target_ssids[i]) != NULL) trigger = true;
        }
        if (trigger && temp_ssid[0] != '\0') {
            for (int i = 0; i < num_ignore_names; i++) {
                if (strcasestr(temp_ssid, ignore_names[i]) != NULL) { trigger = false; break; }
            }
        }
    }
    // --- DATA FRAME PROCESSING ---
    else if (type == WIFI_PKT_DATA) {
        // 1. Check Source MAC
        if ((mac_src[1] == 0x30 && mac_src[2] == 0x44 && (mac_src[0] == 0x00 || (mac_src[0] & 0x02))) || // Cradlepoint Base
            (mac_src[1] == 0xE0 && mac_src[2] == 0x1C && (mac_src[0] == 0x00 || (mac_src[0] & 0x02))) || // Cradlepoint Base 2
            (mac_src[1] == 0xA3 && mac_src[2] == 0x31 && (mac_src[0] == 0x28 || (mac_src[0] & 0x02))) || // Sierra Wireless Base
            (mac_src[1] == 0x14 && mac_src[2] == 0x3E && (mac_src[0] == 0x00 || (mac_src[0] & 0x02)))) { // Airlink Base
            trigger = true;
        } else {
            for (int i = 0; i < num_target_ouis; i++) {
                if (memcmp(mac_src, target_ouis[i].oui, 3) == 0) {
                    trigger = true;
                    break;
                }
            }
        }

        // 2. If not triggered by Source, check Destination MAC
        if (!trigger) {
            if ((mac_dst[1] == 0x30 && mac_dst[2] == 0x44 && (mac_dst[0] == 0x00 || (mac_dst[0] & 0x02))) || // Cradlepoint Base
                (mac_dst[1] == 0xE0 && mac_dst[2] == 0x1C && (mac_dst[0] == 0x00 || (mac_dst[0] & 0x02))) || // Cradlepoint Base 2
                (mac_dst[1] == 0xA3 && mac_dst[2] == 0x31 && (mac_dst[0] == 0x28 || (mac_dst[0] & 0x02))) || // Sierra Wireless Base
                (mac_dst[1] == 0x14 && mac_dst[2] == 0x3E && (mac_dst[0] == 0x00 || (mac_dst[0] & 0x02)))) { // Airlink Base
                trigger = true;
                mac_to_log = mac_dst; // Log the target destination instead of the sender
            } else {
                for (int i = 0; i < num_target_ouis; i++) {
                    if (memcmp(mac_dst, target_ouis[i].oui, 3) == 0) {
                        trigger = true;
                        mac_to_log = mac_dst; // Log the target destination instead of the sender
                        break;
                    }
                }
            }
        }
    }

    update_device_record(wifi_devices, mac_to_log, PROTO_WIFI, pkt->rx_ctrl.rssi, pkt->rx_ctrl.channel, pass_subtype, temp_ssid, trigger);

    if (trigger) {
        if (alert_task_handle) xTaskNotifyGive(alert_task_handle);
    }
}

#if SOC_WIFI_SUPPORT_5G
static const uint8_t hop_channels[] = {
    // 2.4 GHz (Non-Overlapping)
    1, 6, 11,
    // 5 GHz (U-NII-1 - Natively Non-Overlapping)
    36, 40, 44, 48,
    // 5 GHz (U-NII-2 - Natively Non-Overlapping)
    52, 56, 60, 64,
    // 5 GHz (U-NII-3 - Natively Non-Overlapping)
    149, 153, 157, 161, 165
};
#else
static const uint8_t hop_channels[] = {
    // 2.4 GHz (Non-Overlapping)
    1, 6, 11
};
#endif

const int num_hop_channels = sizeof(hop_channels) / sizeof(hop_channels[0]);


void wifi_channel_hopper(void *pvParameters) {
    int idx = 0;
    while (1) {
        esp_wifi_set_channel(hop_channels[idx], WIFI_SECOND_CHAN_NONE);
        idx++;
        if (idx >= num_hop_channels) {
            idx = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
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

        // Update in ble_gap_event_cb
        bool trigger = false;
        if ((mac[1] == 0x30 && mac[2] == 0x44 && (mac[0] == 0x00 || (mac[0] & 0x02))) || // Cradlepoint Base
            (mac[1] == 0xE0 && mac[2] == 0x1C && (mac[0] == 0x00 || (mac[0] & 0x02))) || // Cradlepoint Base 2
            (mac[1] == 0xA3 && mac[2] == 0x31 && (mac[0] == 0x28 || (mac[0] & 0x02))) || // Sierra Wireless Base
            (mac[1] == 0x14 && mac[2] == 0x3E && (mac[0] == 0x00 || (mac[0] & 0x02)))) { // Airlink Base
            trigger = true;
        } else {
            for (int i = 0; i < num_target_ouis; i++) {
                if (memcmp(mac, target_ouis[i].oui, 3) == 0) {
                    trigger = true;
                    break;
                }
            }
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

        update_device_record(ble_devices, mac, PROTO_BLE, desc->rssi, 0, 0, dev_name, trigger);

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
        vTaskDelay(pdMS_TO_TICKS(PRINT_INTERVAL_START_MS));
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
            else if (gps_hardware_detected) {
                printf("GPS Status: SEARCHING | Satellites: %02d \n", gps_satellites_tracked);
            }
            printf("--- WI-FI DEVICES (%d) ---\n", wifi_count);
            printf(" MAC Address       | Total | P-Req | P-Rsp | Beacons | RSSI | Chan | SSID \n");
            for (int i = 0; i < (wifi_count < 30 ? wifi_count : 30); i++) {
                printf(" %02x:%02x:%02x:%02x:%02x:%02x | %-5lu | %-5lu | %-5lu | %-7lu | %-4d | %-4u | %s\n",
                       sorted_copy[i].mac[0], sorted_copy[i].mac[1], sorted_copy[i].mac[2],
                       sorted_copy[i].mac[3], sorted_copy[i].mac[4], sorted_copy[i].mac[5],
                       sorted_copy[i].packet_count, sorted_copy[i].probe_req_count,
                       sorted_copy[i].probe_resp_count, sorted_copy[i].beacon_count,
                       sorted_copy[i].strongest_rssi, sorted_copy[i].channel, sorted_copy[i].name_ssid);
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
        vTaskDelay(pdMS_TO_TICKS(PRINT_INTERVAL_END_MS));

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
        if (rxBytes > 0 && !gps_hardware_detected) {
            for (int i = 0; i < rxBytes; i++) {
                if (data[i] == '$') {
                    gps_hardware_detected = true;
                    break;
                }
            }
        }
        for (int i = 0; i < rxBytes; i++) {
            if (data[i] == '\n') {
                line[line_len] = '\0';
                if (strncmp(line, "$GNRMC", 6) == 0 || strncmp(line, "$GPRMC", 6) == 0) {
                    xSemaphoreTake(data_mutex, portMAX_DELAY);
                    gps_satellites_tracked = temp_sats_in_view;
                    temp_sats_in_view = 0;
                    xSemaphoreGive(data_mutex);
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
                        gpio_set_level(LED_PIN, 0); // Blink LED on GPS lock
                        vTaskDelay(pdMS_TO_TICKS(50));
                        gpio_set_level(LED_PIN, 1);
                    } else {
                        xSemaphoreTake(data_mutex, portMAX_DELAY);
                        gps_valid = false;
                        xSemaphoreGive(data_mutex);
                    }
                }
                else if (strlen(line) >= 6 && strncmp(&line[3], "GSV", 3) == 0) {
                    char *tokens[5];
                    int t_cnt = 0;
                    char *ptr = line;
                    char *token = ptr;
                    while (*ptr) {
                        if (*ptr == ',') {
                            *ptr = '\0';
                            tokens[t_cnt++] = token;
                            token = ptr + 1;
                            if (t_cnt >= 4) break;
                        }
                        ptr++;
                    }
                    if (t_cnt < 4) tokens[t_cnt++] = token;

                    // tokens[1] = Total Messages, tokens[2] = Message Number, tokens[3] = Sats in View
                    if (t_cnt >= 4) {
                        int msg_num = atoi(tokens[2]);
                        int sats = atoi(tokens[3]);

                        xSemaphoreTake(data_mutex, portMAX_DELAY);
                        // If it's the first message of a new burst, reset the accumulator
                        if (msg_num == 1) {
                            temp_sats_in_view = sats;
                        } else {
                            temp_sats_in_view += sats;
                        }
                        gps_satellites_tracked = temp_sats_in_view;
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

void boot_beep(int beeps) {
    gpio_set_direction(BUZZER_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    for (int i = 0; i < beeps; i++) {
        gpio_set_level(BUZZER_PIN, 1);
        gpio_set_level(LED_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
        gpio_set_level(BUZZER_PIN, 0);
        gpio_set_level(LED_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}
/* ========================================================================== *
 * ENTRY                                                                      *
 * ========================================================================== */

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());

    esp_vfs_spiffs_conf_t conf = { .base_path = "/spiffs", .partition_label = NULL, .max_files = 5, .format_if_mount_failed = true };
    esp_vfs_spiffs_register(&conf);

    // Clear logged targets on boot
    //remove("/spiffs/targets.txt");

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

    // wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA }; // also data
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    // Bluetooth init
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(ble_host_task);

    xTaskCreate(gps_task, "gps_task", 3072, NULL, 5, NULL);
    xTaskCreate(wifi_channel_hopper, "wifi_hop", 1536, NULL, 5, NULL);
    xTaskCreate(monitor_print_task, "printer", 3072, NULL, 4, NULL);

    printf("System Tick Rate: %d Hz\n", configTICK_RATE_HZ);
    printf("Tick Period: %lu ms\n", portTICK_PERIOD_MS);
    printf("Max Devices: %d \n", MAX_DEVICES);

    oled_mutex = xSemaphoreCreateMutex();
    init_oled();
    xTaskCreate(oled_task, "oled_task", 2048, NULL, 3, NULL);

    printf("Checking for GPS hardware...\n");
    for (int i = 0; i < 15; i++) {
        if (gps_hardware_detected) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (gps_hardware_detected) {
        printf("--> GPS: FOUND\n");
    } else {
        printf("--> GPS: NOT FOUND\n");
    }
    if (has_screen) {
        printf("--> OLED: FOUND\n");
    } else {
        printf("--> OLED: NOT FOUND\n");
    }

    // Beep on boot to know it booted
    boot_beep(3);
}
