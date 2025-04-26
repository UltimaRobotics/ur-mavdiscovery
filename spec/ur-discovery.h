#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <libudev.h>
#include <dirent.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <cJSON.h>
#include <cssl.h>
#include <libmavlink.h>
#include <ur-rpc-template.h>


#ifdef __GNUC__
#define WEAK __attribute__((weak))
#else
#define WEAK
#endif

#define EVENT_SIZE  (sizeof(struct inotify_event))
#define BUF_LEN     (1024 * (EVENT_SIZE + 16))
#define MAX_TEMPLATES 32
#define MAX_TEMPLATE_LEN 64
#define DEV_PATH_LEN 256
#define MAVLINK_TIMEOUT_MS 2500
#define MAX_DEVICES 100
#define HEARTBEAT_REQUEST_INTERVAL_MS 500
#define INFO_COLLECTION_TIMEOUT_MS 3000

// Structure to hold collected PX4 device information
typedef struct {
    uint64_t flight_sw_version;
    uint64_t middleware_sw_version;
    uint64_t os_sw_version;
    uint64_t board_version;
    uint16_t vendor_id;
    uint16_t product_id;
    char flight_custom_version[8];
    char middleware_custom_version[8];
    char os_custom_version[8];
    char uid[18]; // 12 bytes as hex string + null terminator
    char product_name[20];
    char manufacturer[20];
} PX4DeviceInfo;

typedef struct {
    char path[DEV_PATH_LEN];
    bool mavlink_valid;
    pthread_t thread;
    bool thread_running;
    cssl_t *serial;
    int id;
    bool heartbeat_received;
    bool info_collected;
    PX4DeviceInfo px4_info;
    time_t info_request_time;
} DeviceInfo;

typedef struct {
    char templates[MAX_TEMPLATES][MAX_TEMPLATE_LEN];
    int count;
} DeviceTemplates;

// Process autopilot version information
typedef struct {
    uint16_t vendor_id;
    uint16_t product_id;
    const char *manufacturer;
    const char *product_name;
} DeviceIdentifier;

// Known MAVLink flight controllers
static const DeviceIdentifier known_devices[] = {
    // PX4/Pixhawk family
    {0x26AC, 0x0011, "3DR", "Pixhawk 1"},
    {0x26AC, 0x0012, "3DR", "Pixhawk 2"},
    {0x26AC, 0x0013, "3DR", "Pixhawk 3"},
    {0x26AC, 0x0014, "3DR", "Pixhawk 4"},
    {0x26AC, 0x0015, "3DR", "Pixhawk 5"},
    {0x26AC, 0x0016, "3DR", "Pixhawk 6"},
    {0x26AC, 0x0017, "3DR", "Pixhawk Mini"},
    {0x26AC, 0x0018, "3DR", "Pixhawk Nano"},
    {0x26AC, 0x0019, "3DR", "Pixhawk Micro"},
    
    // Holybro
    {0x16D0, 0x0DBA, "Holybro", "Pixhawk 4"},
    {0x16D0, 0x0DBB, "Holybro", "Pixhawk 4 Mini"},
    {0x16D0, 0x0DBC, "Holybro", "Pixhawk 5X"},
    {0x16D0, 0x0DBD, "Holybro", "Pixhawk 6X"},
    {0x16D0, 0x0DBE, "Holybro", "Pixhawk 6C"},
    {0x16D0, 0x0DBF, "Holybro", "Pix32 v5"},
    {0x16D0, 0x0DC0, "Holybro", "Pix32 v6"},
    
    // CUAV
    {0x1FC9, 0x0001, "CUAV", "Pixhack v3"},
    {0x1FC9, 0x0002, "CUAV", "Pixhack v5"},
    {0x1FC9, 0x0003, "CUAV", "X7"},
    {0x1FC9, 0x0004, "CUAV", "Nora"},
    {0x1FC9, 0x0005, "CUAV", "V5+"},
    {0x1FC9, 0x0006, "CUAV", "V5 Nano"},
    
    // ArduPilot
    {0x0483, 0x5740, "ArduPilot", "ChibiOS"},
    {0x1209, 0x5740, "ArduPilot", "PX4"},
    
    // Hex/ProfiCNC
    {0x1209, 0x5741, "Hex", "Pixhawk 2.1"},
    {0x1209, 0x5742, "Hex", "Pixracer"},
    {0x1209, 0x5743, "Hex", "Pixhawk Cube"},
    
    // mRo
    {0x1209, 0x5744, "mRo", "Pixhawk 1"},
    {0x1209, 0x5745, "mRo", "X2.1"},
    {0x1209, 0x5746, "mRo", "Control Zero"},
    
    // Auterion
    {0x1209, 0x5747, "Auterion", "Skynode"},
    
    // ModalAI
    {0x1209, 0x5748, "ModalAI", "Flight Core v1"},
    {0x1209, 0x5749, "ModalAI", "Flight Core v2"},
    
    // Intel Aero
    {0x8086, 0x0AF5, "Intel", "Aero Ready to Fly Drone"},
    
    // Snapdragon Flight
    {0x05BA, 0x0011, "Qualcomm", "Snapdragon Flight"},
    
    // DJI
    {0x2CA3, 0x0010, "DJI", "N3"},
    {0x2CA3, 0x0011, "DJI", "A3"},
    {0x2CA3, 0x0012, "DJI", "M600"},
    
    // Zero defaults at the end
    {0x0000, 0x0000, "Unknown", "Unknown"}
};


static DeviceInfo devices[MAX_DEVICES];
static int device_count = 0;
static pthread_mutex_t devices_mutex = PTHREAD_MUTEX_INITIALIZER;

bool load_templates_from_json(const char *filename, DeviceTemplates *templates);
bool is_monitored_device(const char *devname, const DeviceTemplates *templates);
void send_heartbeat_request(cssl_t *serial);
void send_autopilot_version_request(cssl_t *serial);
void identify_device(DeviceInfo *dev);
void process_autopilot_version(mavlink_message_t *msg, DeviceInfo *dev);
void print_px4_device_info(DeviceInfo *dev);
void mavlink_callback(int id, uint8_t *buf, int length);

void* check_mavlink_device(void *arg);
void start_mavlink_check(const char *devpath);

void print_device_info(const char *devname);
void scan_existing_devices(const DeviceTemplates *templates);
void print_usage(const char *program_name);
void cleanup_threads();
void register_device_mavrouter(char* dev_path);
void unregister_device_mavrouter(char* dev_path);

#ifdef _DevCollecterAdvanced
    #define DEV_PATH_LEN 256
    #define MAX_STRING_LEN 256

    typedef struct {
        char dev_path[DEV_PATH_LEN];
        char dev_name[MAX_STRING_LEN];
        char vid[MAX_STRING_LEN];
        char pid[MAX_STRING_LEN];
        char manufacturer[MAX_STRING_LEN];
        char product[MAX_STRING_LEN];
        char serial[MAX_STRING_LEN];
        bool usb_info_available;
    } DeviceInfoTransport;

    char* serialize_device_info_transport(const DeviceInfoTransport* info);
    void free_device_info_transport(DeviceInfoTransport* info);
#endif
