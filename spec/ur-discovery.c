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
#include <ur-discovery.h>

#define MAVROUTER_ACTIONS_TOPIC "ur-mavrouter-actions"
#define MAVROUTER_RESULTS_TOPIC "ur-mavrouter-results"
#define MAVROUTER_FORWARDER_TOPIC "ur-linker-info"



void register_device_mavrouter(char* dev_path){
    DeviceState state = {
        .dev_path = dev_path,
        .enable =true
    };
    char* json ;
    serialize_device_state(&state,json,MAVROUTER_ACTIONS_TOPIC);
    free(json);
}
void unregister_device_mavrouter(char* dev_path){
    DeviceState state = {
        .dev_path = dev_path,
        .enable =false
    };
    char* json ;
    serialize_device_state(&state,json,MAVROUTER_ACTIONS_TOPIC);
    free(json);
}

#ifdef _DevCollecterAdvanced
    char* serialize_device_info_transport(const DeviceInfoTransport* info) {
        if (!info) {
            fprintf(stderr, "Error: NULL device info pointer\n");
            return NULL;
        }

        // Create root JSON object
        cJSON *root = cJSON_CreateObject();
        if (!root) {
            fprintf(stderr, "Error: Failed to create JSON object\n");
            return NULL;
        }

        // Add basic device info (always present)
        cJSON_AddStringToObject(root, "dev_path", info->dev_path);
        cJSON_AddStringToObject(root, "dev_name", info->dev_name);
        
        // Add USB-specific info if available
        if (info->usb_info_available) {
            cJSON_AddStringToObject(root, "vid", info->vid);
            cJSON_AddStringToObject(root, "pid", info->pid);
            cJSON_AddStringToObject(root, "manufacturer", info->manufacturer);
            cJSON_AddStringToObject(root, "product", info->product);
            cJSON_AddStringToObject(root, "serial", info->serial);
        }
        
        // Add availability flag
        cJSON_AddBoolToObject(root, "usb_info_available", info->usb_info_available);

        // Generate JSON string
        char* json_str = cJSON_PrintUnformatted(root);  // or cJSON_Print for formatted
        if (!json_str) {
            fprintf(stderr, "Error: Failed to print JSON\n");
            cJSON_Delete(root);
            return NULL;
        }

        // Clean up cJSON structure
        cJSON_Delete(root);
        
        return json_str;  // Caller must free this with cJSON_free
    }

    void free_device_info_transport(DeviceInfoTransport* info) {
        if (info) {
            free(info);
        }
    }
#endif

bool load_templates_from_json(const char *filename, DeviceTemplates *templates) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        perror("Failed to open config file");
        return false;
    }

    fseek(fp, 0, SEEK_END);
    long length = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *data = malloc(length + 1);
    fread(data, 1, length, fp);
    fclose(fp);
    data[length] = '\0';

    cJSON *root = cJSON_Parse(data);
    free(data);
    if (!root) {
        fprintf(stderr, "Error parsing JSON: %s\n", cJSON_GetErrorPtr());
        return false;
    }

    cJSON *allowed = cJSON_GetObjectItemCaseSensitive(root, "allowed_templates");
    if (!cJSON_IsArray(allowed)) {
        fprintf(stderr, "Error: 'allowed_templates' is not an array\n");
        cJSON_Delete(root);
        return false;
    }

    templates->count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, allowed) {
        if (templates->count >= MAX_TEMPLATES) {
            fprintf(stderr, "Warning: Too many templates, maximum is %d\n", MAX_TEMPLATES);
            break;
        }

        if (!cJSON_IsString(item)) {
            fprintf(stderr, "Warning: Non-string value in allowed_templates array\n");
            continue;
        }

        const char *template = item->valuestring;
        if (strlen(template) >= MAX_TEMPLATE_LEN) {
            fprintf(stderr, "Warning: Template too long, maximum is %d characters\n", MAX_TEMPLATE_LEN-1);
            continue;
        }

        strncpy(templates->templates[templates->count], template, MAX_TEMPLATE_LEN-1);
        templates->templates[templates->count][MAX_TEMPLATE_LEN-1] = '\0';
        templates->count++;
    }

    cJSON_Delete(root);
    return true;
}

bool is_monitored_device(const char *devname, const DeviceTemplates *templates) {
    for (int i = 0; i < templates->count; i++) {
        const char *pattern = templates->templates[i];
        size_t pattern_len = strlen(pattern);
        
        if (pattern[pattern_len-1] == '*') {
            if (strncmp(devname, pattern, pattern_len-1) == 0) {
                return true;
            }
        } else {
            if (strcmp(devname, pattern) == 0) {
                return true;
            }
        }
    }
    return false;
}

void send_heartbeat_request(cssl_t *serial) {
    mavlink_message_t msg;
    uint8_t buf[MAVLINK_MAX_PACKET_LEN];
    
    mavlink_msg_heartbeat_pack(0, 0, &msg, 
                              MAV_TYPE_GENERIC, 
                              MAV_AUTOPILOT_INVALID, 
                              0, 0, 0);
    
    uint16_t len = mavlink_msg_to_send_buffer(buf, &msg);
    cssl_putdata(serial, buf, len);
}

// Send request for autopilot version information
void send_autopilot_version_request(cssl_t *serial) {
    mavlink_message_t msg;
    uint8_t buf[MAVLINK_MAX_PACKET_LEN];
    
    mavlink_msg_command_long_pack(0, 0, &msg, 
                                1, 1, // target system, component
                                MAV_CMD_REQUEST_MESSAGE,
                                0, // confirmation
                                MAVLINK_MSG_ID_AUTOPILOT_VERSION,
                                0, 0, 0, 0, 0, 0);
    uint16_t len = mavlink_msg_to_send_buffer(buf, &msg);
    cssl_putdata(serial, buf, len);
}


// Get manufacturer and product name from vendor/product IDs
// Get manufacturer and product name from vendor/product IDs
void identify_device(DeviceInfo *dev) {
    // Check against known devices first
    for (int i = 0; known_devices[i].vendor_id != 0 || known_devices[i].product_id != 0; i++) {
        if (known_devices[i].vendor_id == dev->px4_info.vendor_id &&
            known_devices[i].product_id == dev->px4_info.product_id) {
            strncpy(dev->px4_info.manufacturer, known_devices[i].manufacturer, 
                   sizeof(dev->px4_info.manufacturer));
            strncpy(dev->px4_info.product_name, known_devices[i].product_name,
                   sizeof(dev->px4_info.product_name));
            return;
        }
    }
    
    // Fallback to QGCUsbId mappings
    switch (dev->px4_info.vendor_id) {
        case 0x26AC: // PX4
            strncpy(dev->px4_info.manufacturer, "PX4", sizeof(dev->px4_info.manufacturer));
            switch (dev->px4_info.product_id) {
                case 0x0010: strncpy(dev->px4_info.product_name, "PX4FMU v1", sizeof(dev->px4_info.product_name)); break;
                case 0x0011: strncpy(dev->px4_info.product_name, "PX4FMU v2/v3", sizeof(dev->px4_info.product_name)); break;
                case 0x0012: strncpy(dev->px4_info.product_name, "PX4FMU v4", sizeof(dev->px4_info.product_name)); break;
                case 0x0013: strncpy(dev->px4_info.product_name, "PX4FMU v4PRO", sizeof(dev->px4_info.product_name)); break;
                case 0x0032: strncpy(dev->px4_info.product_name, "PX4FMU v5", sizeof(dev->px4_info.product_name)); break;
                case 0x0033: strncpy(dev->px4_info.product_name, "PX4FMU v5X", sizeof(dev->px4_info.product_name)); break;
                case 0x0038: strncpy(dev->px4_info.product_name, "PX4FMU v6C", sizeof(dev->px4_info.product_name)); break;
                case 0x0036: strncpy(dev->px4_info.product_name, "PX4FMU v6U", sizeof(dev->px4_info.product_name)); break;
                case 0x0035: strncpy(dev->px4_info.product_name, "PX4FMU v6X", sizeof(dev->px4_info.product_name)); break;
                case 0x001D: strncpy(dev->px4_info.product_name, "PX4FMU v6XRT", sizeof(dev->px4_info.product_name)); break;
                case 0x0030: strncpy(dev->px4_info.product_name, "MindPX v2", sizeof(dev->px4_info.product_name)); break;
                default: strncpy(dev->px4_info.product_name, "Unknown PX4", sizeof(dev->px4_info.product_name)); break;
            }
            break;
            
        case 0x1546: // u-blox
            strncpy(dev->px4_info.manufacturer, "u-blox", sizeof(dev->px4_info.manufacturer));
            switch (dev->px4_info.product_id) {
                case 0x01a5: strncpy(dev->px4_info.product_name, "u-blox 5", sizeof(dev->px4_info.product_name)); break;
                case 0x01a6: strncpy(dev->px4_info.product_name, "u-blox 6", sizeof(dev->px4_info.product_name)); break;
                case 0x01a7: strncpy(dev->px4_info.product_name, "u-blox 7", sizeof(dev->px4_info.product_name)); break;
                case 0x01a8: strncpy(dev->px4_info.product_name, "u-blox 8", sizeof(dev->px4_info.product_name)); break;
                default: strncpy(dev->px4_info.product_name, "Unknown u-blox", sizeof(dev->px4_info.product_name)); break;
            }
            break;
            
        case 0x20A0: // OpenPilot
            strncpy(dev->px4_info.manufacturer, "OpenPilot", sizeof(dev->px4_info.manufacturer));
            switch (dev->px4_info.product_id) {
                case 0x415E: strncpy(dev->px4_info.product_name, "Revolution", sizeof(dev->px4_info.product_name)); break;
                case 0x415C: strncpy(dev->px4_info.product_name, "OPLink", sizeof(dev->px4_info.product_name)); break;
                case 0x41D0: strncpy(dev->px4_info.product_name, "Sparky2", sizeof(dev->px4_info.product_name)); break;
                case 0x415D: strncpy(dev->px4_info.product_name, "CC3D", sizeof(dev->px4_info.product_name)); break;
                default: strncpy(dev->px4_info.product_name, "Unknown OpenPilot", sizeof(dev->px4_info.product_name)); break;
            }
            break;
            
        case 0x0483: // STMicroelectronics
            strncpy(dev->px4_info.manufacturer, "STMicroelectronics", sizeof(dev->px4_info.manufacturer));
            strncpy(dev->px4_info.product_name, "Unknown STM", sizeof(dev->px4_info.product_name));
            break;
            
        case 0x1209: // ArduPilot
            strncpy(dev->px4_info.manufacturer, "ArduPilot", sizeof(dev->px4_info.manufacturer));
            switch (dev->px4_info.product_id) {
                case 0x5740: strncpy(dev->px4_info.product_name, "ChibiOS", sizeof(dev->px4_info.product_name)); break;
                case 0x5741: strncpy(dev->px4_info.product_name, "ChibiOS2", sizeof(dev->px4_info.product_name)); break;
                default: strncpy(dev->px4_info.product_name, "Unknown ArduPilot", sizeof(dev->px4_info.product_name)); break;
            }
            break;
            
        case 0x1FC9: // DragonLink
            strncpy(dev->px4_info.manufacturer, "DragonLink", sizeof(dev->px4_info.manufacturer));
            if (dev->px4_info.product_id == 0x0083) {
                strncpy(dev->px4_info.product_name, "DragonLink", sizeof(dev->px4_info.product_name));
            } else {
                strncpy(dev->px4_info.product_name, "Unknown DragonLink", sizeof(dev->px4_info.product_name));
            }
            break;
            
        case 0x2DAE: // CubePilot
            strncpy(dev->px4_info.manufacturer, "CubePilot", sizeof(dev->px4_info.manufacturer));
            switch (dev->px4_info.product_id) {
                case 0x1011: strncpy(dev->px4_info.product_name, "Cube Black/Black+", sizeof(dev->px4_info.product_name)); break;
                case 0x1001: strncpy(dev->px4_info.product_name, "Cube Black Bootloader", sizeof(dev->px4_info.product_name)); break;
                case 0x1016: strncpy(dev->px4_info.product_name, "Cube Orange", sizeof(dev->px4_info.product_name)); break;
                case 0x1017: strncpy(dev->px4_info.product_name, "Cube Orange2", sizeof(dev->px4_info.product_name)); break;
                case 0x1058: strncpy(dev->px4_info.product_name, "Cube Orange+", sizeof(dev->px4_info.product_name)); break;
                case 0x1002: strncpy(dev->px4_info.product_name, "Cube Yellow Bootloader", sizeof(dev->px4_info.product_name)); break;
                case 0x1012: strncpy(dev->px4_info.product_name, "Cube Yellow", sizeof(dev->px4_info.product_name)); break;
                case 0x1005: strncpy(dev->px4_info.product_name, "Cube Purple Bootloader", sizeof(dev->px4_info.product_name)); break;
                case 0x1015: strncpy(dev->px4_info.product_name, "Cube Purple", sizeof(dev->px4_info.product_name)); break;
                default: strncpy(dev->px4_info.product_name, "Unknown Cube", sizeof(dev->px4_info.product_name)); break;
            }
            break;
            
        case 0x3163: // CUAV
            strncpy(dev->px4_info.manufacturer, "CUAV", sizeof(dev->px4_info.manufacturer));
            if (dev->px4_info.product_id == 0x004C) {
                strncpy(dev->px4_info.product_name, "Nora/X7Pro", sizeof(dev->px4_info.product_name));
            } else {
                strncpy(dev->px4_info.product_name, "Unknown CUAV", sizeof(dev->px4_info.product_name));
            }
            break;
            
        case 0x3162: // Holybro
            strncpy(dev->px4_info.manufacturer, "Holybro", sizeof(dev->px4_info.manufacturer));
            switch (dev->px4_info.product_id) {
                case 0x0047: strncpy(dev->px4_info.product_name, "Pixhawk4", sizeof(dev->px4_info.product_name)); break;
                case 0x0049: strncpy(dev->px4_info.product_name, "PH4 Mini", sizeof(dev->px4_info.product_name)); break;
                case 0x004B: strncpy(dev->px4_info.product_name, "Durandal", sizeof(dev->px4_info.product_name)); break;
                default: strncpy(dev->px4_info.product_name, "Unknown Holybro", sizeof(dev->px4_info.product_name)); break;
            }
            break;
            
        case 0x27AC: // Laser Navigation
            strncpy(dev->px4_info.manufacturer, "Laser Navigation", sizeof(dev->px4_info.manufacturer));
            switch (dev->px4_info.product_id) {
                case 0x1151: strncpy(dev->px4_info.product_name, "VRBrain v51", sizeof(dev->px4_info.product_name)); break;
                case 0x1152: strncpy(dev->px4_info.product_name, "VRBrain v52", sizeof(dev->px4_info.product_name)); break;
                case 0x1154: strncpy(dev->px4_info.product_name, "VRBrain v54", sizeof(dev->px4_info.product_name)); break;
                case 0x1910: strncpy(dev->px4_info.product_name, "VRCore v10", sizeof(dev->px4_info.product_name)); break;
                case 0x1351: strncpy(dev->px4_info.product_name, "VRUBrain v51", sizeof(dev->px4_info.product_name)); break;
                default: strncpy(dev->px4_info.product_name, "Unknown VRBrain", sizeof(dev->px4_info.product_name)); break;
            }
            break;
            
        default:
            // Fallback for unknown devices
            strncpy(dev->px4_info.manufacturer, "Unknown", sizeof(dev->px4_info.manufacturer));
            strncpy(dev->px4_info.product_name, "Unknown", sizeof(dev->px4_info.product_name));
            
            // Try to identify by board version if vendor/product ID is unknown
            switch ((dev->px4_info.board_version >> 16) & 0xFFFF) { // Board type is in upper 16 bits
                case 0x0009: // PX4_BOARD_PIXHAWK
                    strncpy(dev->px4_info.manufacturer, "3DR", sizeof(dev->px4_info.manufacturer));
                    strncpy(dev->px4_info.product_name, "Pixhawk 1", sizeof(dev->px4_info.product_name));
                    break;
                case 0x0010: // PX4_BOARD_PIXHAWK2
                    strncpy(dev->px4_info.manufacturer, "3DR", sizeof(dev->px4_info.manufacturer));
                    strncpy(dev->px4_info.product_name, "Pixhawk 2", sizeof(dev->px4_info.product_name));
                    break;
                case 0x0015: // PX4_BOARD_PIXRACER
                    strncpy(dev->px4_info.manufacturer, "Hex", sizeof(dev->px4_info.manufacturer));
                    strncpy(dev->px4_info.product_name, "Pixracer", sizeof(dev->px4_info.product_name));
                    break;
                case 0x0016: // PX4_BOARD_PIXHAWK3_PRO
                    strncpy(dev->px4_info.manufacturer, "mRo", sizeof(dev->px4_info.manufacturer));
                    strncpy(dev->px4_info.product_name, "Pixhawk 3 Pro", sizeof(dev->px4_info.product_name));
                    break;
                case 0x0017: // PX4_BOARD_PIXHAWK4
                    strncpy(dev->px4_info.manufacturer, "Holybro", sizeof(dev->px4_info.manufacturer));
                    strncpy(dev->px4_info.product_name, "Pixhawk 4", sizeof(dev->px4_info.product_name));
                    break;
                case 0x0018: // PX4_BOARD_PIXHAWK4_PRO
                    strncpy(dev->px4_info.manufacturer, "Holybro", sizeof(dev->px4_info.manufacturer));
                    strncpy(dev->px4_info.product_name, "Pixhawk 4 Pro", sizeof(dev->px4_info.product_name));
                    break;
                case 0x0019: // PX4_BOARD_PIXHAWK5X
                    strncpy(dev->px4_info.manufacturer, "Holybro", sizeof(dev->px4_info.manufacturer));
                    strncpy(dev->px4_info.product_name, "Pixhawk 5X", sizeof(dev->px4_info.product_name));
                    break;
                case 0x001A: // PX4_BOARD_PIXHAWK6X
                    strncpy(dev->px4_info.manufacturer, "Holybro", sizeof(dev->px4_info.manufacturer));
                    strncpy(dev->px4_info.product_name, "Pixhawk 6X", sizeof(dev->px4_info.product_name));
                    break;
            }
            break;
    }
}

// Updated process_autopilot_version function
void process_autopilot_version(mavlink_message_t *msg, DeviceInfo *dev) {
    mavlink_autopilot_version_t version;
    mavlink_msg_autopilot_version_decode(msg, &version);
    
    // Store version information
    dev->px4_info.flight_sw_version = version.flight_sw_version;
    dev->px4_info.middleware_sw_version = version.middleware_sw_version;
    dev->px4_info.os_sw_version = version.os_sw_version;
    dev->px4_info.board_version = version.board_version;
    dev->px4_info.vendor_id = version.vendor_id;
    dev->px4_info.product_id = version.product_id;
    
    // Copy custom version strings
    memcpy(dev->px4_info.flight_custom_version, version.flight_custom_version, 8);
    memcpy(dev->px4_info.middleware_custom_version, version.middleware_custom_version, 8);
    memcpy(dev->px4_info.os_custom_version, version.os_custom_version, 8);
    
    // Convert UID to hex string
    if (version.uid2[0] != 0) {
        // Use uid2 if available
        for (int i = 0; i < 12; i++) {
            sprintf(&dev->px4_info.uid[i*2], "%02X", version.uid2[i]);
        }
    } else {
        // Fall back to uid if uid2 is not available
        uint64_t uid = version.uid;
        for (int i = 0; i < 8; i++) {
            sprintf(&dev->px4_info.uid[i*2], "%02X", (unsigned)(uid & 0xFF));
            uid >>= 8;
        }
    }
    dev->px4_info.uid[16] = '\0';
    
    // Identify manufacturer and product
    identify_device(dev);
    
    dev->info_collected = true;
    printf("PX4 device information collected from %s\n", dev->path);
}

#include <inttypes.h> 

char* serialize_px4_device_info(const PX4DeviceInfo* info) {
    if (!info) return NULL;

    // Calculate required buffer size (with margin for safety)
    size_t buf_size = 512;  // Initial estimate
    char* buffer = malloc(buf_size);
    if (!buffer) return NULL;

    // Format as JSON
    int written = snprintf(buffer, buf_size,
        "{"
        "\"flight_sw_version\":%" PRIu64 ","
        "\"middleware_sw_version\":%" PRIu64 ","
        "\"os_sw_version\":%" PRIu64 ","
        "\"board_version\":%" PRIu64 ","
        "\"vendor_id\":%u,"
        "\"product_id\":%u,"
        "\"flight_custom_version\":\"%.8s\","
        "\"middleware_custom_version\":\"%.8s\","
        "\"os_custom_version\":\"%.8s\","
        "\"uid\":\"%s\","
        "\"product_name\":\"%s\","
        "\"manufacturer\":\"%s\""
        "}",
        info->flight_sw_version,
        info->middleware_sw_version,
        info->os_sw_version,
        info->board_version,
        info->vendor_id,
        info->product_id,
        info->flight_custom_version,
        info->middleware_custom_version,
        info->os_custom_version,
        info->uid,
        info->product_name,
        info->manufacturer);

    // Handle buffer overflow
    if (written < 0) {
        free(buffer);
        return NULL;
    }

    if ((size_t)written >= buf_size) {
        // Try again with exact required size
        buf_size = (size_t)written + 1;
        char* new_buffer = realloc(buffer, buf_size);
        if (!new_buffer) {
            free(buffer);
            return NULL;
        }
        buffer = new_buffer;
        
        snprintf(buffer, buf_size,
            "{"
            "\"flight_sw_version\":%" PRIu64 ","
            "\"middleware_sw_version\":%" PRIu64 ","
            "\"os_sw_version\":%" PRIu64 ","
            "\"board_version\":%" PRIu64 ","
            "\"vendor_id\":%u,"
            "\"product_id\":%u,"
            "\"flight_custom_version\":\"%.8s\","
            "\"middleware_custom_version\":\"%.8s\","
            "\"os_custom_version\":\"%.8s\","
            "\"uid\":\"%s\","
            "\"product_name\":\"%.20s\","
            "\"manufacturer\":\"%.20s\""
            "}",
            info->flight_sw_version,
            info->middleware_sw_version,
            info->os_sw_version,
            info->board_version,
            info->vendor_id,
            info->product_id,
            info->flight_custom_version,
            info->middleware_custom_version,
            info->os_custom_version,
            info->uid,
            info->product_name,
            info->manufacturer);
    }

    return buffer;
}

// Print collected PX4 device information
void print_px4_device_info(DeviceInfo *dev) {
    printf("\nPX4 Device Information for %s:\n", dev->path);
    printf("  Manufacturer: %s\n", dev->px4_info.manufacturer);
    printf("  Product: %s\n", dev->px4_info.product_name);
    printf("  Flight SW Version: %llu\n", dev->px4_info.flight_sw_version);
    printf("  Middleware SW Version: %llu\n", dev->px4_info.middleware_sw_version);
    printf("  OS SW Version: %llu\n", dev->px4_info.os_sw_version);
    printf("  Board Version: %llu\n", dev->px4_info.board_version);
    printf("  Vendor ID: 0x%04X\n", dev->px4_info.vendor_id);
    printf("  Product ID: 0x%04X\n", dev->px4_info.product_id);
    printf("  UID: %s\n", dev->px4_info.uid);
    printf("  Flight Custom Version: %.8s\n", dev->px4_info.flight_custom_version);
    printf("  Middleware Custom Version: %.8s\n", dev->px4_info.middleware_custom_version);
    printf("  OS Custom Version: %.8s\n", dev->px4_info.os_custom_version);

    char* json = serialize_px4_device_info(dev);
    publish_to_custom_topic(MAVROUTER_FORWARDER_TOPIC,json);
    free(json);
}

// Updated mavlink_callback to handle additional messages
void mavlink_callback(int id, uint8_t *buf, int length) {
    if (!(length > 0)) {
        printf("[+]Error while parsing String");
        return;
    }

    mavlink_message_t msg;
    mavlink_status_t status;
    
    for(int i = 0; i < length; i++) {
        if (mavlink_parse_char(MAVLINK_COMM_0, buf[i], &msg, &status)) {
            pthread_mutex_lock(&devices_mutex);
            for (int j = 0; j < device_count; j++) {
                if (devices[j].id == id) {
                    switch(msg.msgid) {
                        case MAVLINK_MSG_ID_HEARTBEAT:
                            if (!devices[j].heartbeat_received) {
                                devices[j].heartbeat_received = true;
                                devices[j].mavlink_valid = true;
                                printf("MAVLink heartbeat received from %s\n", devices[j].path);
                                // register_device_mavrouter(devices[j].path);

                                send_autopilot_version_request(devices[j].serial);
                                devices[j].info_request_time = time(NULL);
                            }
                            break;
                            
                        case MAVLINK_MSG_ID_AUTOPILOT_VERSION:
                            if (!devices[j].info_collected) {
                                process_autopilot_version(&msg, &devices[j]);
                                print_px4_device_info(&devices[j]);
                            }
                            break;
                            
                        default:
                            // Handle other message types if needed
                            break;
                    }
                    break;
                }
            }
            pthread_mutex_unlock(&devices_mutex);
        }
    }
}

// Updated check_mavlink_device to include info collection timeout
void* check_mavlink_device(void *arg) {
    DeviceInfo *dev = (DeviceInfo *)arg;
    struct timespec start, now, last_request;
    bool timeout = false;
    bool first_request = true;
    bool info_timeout = false;

    clock_gettime(CLOCK_MONOTONIC, &start);
    clock_gettime(CLOCK_MONOTONIC, &last_request);
    
    printf("Starting MAVLink check for %s (ID: %d)\n", dev->path, dev->id);
    
    cssl_start();
    dev->serial = cssl_open(dev->path, mavlink_callback, dev->id, 115200, 8, 0, 1);
    
    if (!dev->serial) {
        fprintf(stderr, "Failed to open serial port %s\n", dev->path);
        dev->thread_running = false;
        return NULL;
    }
    
    while (!timeout && !dev->mavlink_valid) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 + 
                         (now.tv_nsec - start.tv_nsec) / 1000000;
        
        // Send heartbeat request periodically
        long since_last_request = (now.tv_sec - last_request.tv_sec) * 1000 +
                                 (now.tv_nsec - last_request.tv_nsec) / 1000000;
        
        if (first_request || since_last_request >= HEARTBEAT_REQUEST_INTERVAL_MS) {
            send_heartbeat_request(dev->serial);
            clock_gettime(CLOCK_MONOTONIC, &last_request);
            first_request = false;
        }
        
        if (elapsed_ms >= MAVLINK_TIMEOUT_MS) {
            timeout = true;
        }
        usleep(10000); // 10ms sleep
    }

    // If we found a MAVLink device, wait for info collection
    if (dev->mavlink_valid) {
        printf("Device %s is MAVLink compatible - collecting info...\n", dev->path);
        register_device_mavrouter(dev->path);
        time_t info_start = time(NULL);
        
        while (!info_timeout && !dev->info_collected) {
            time_t current = time(NULL);
            if (difftime(current, info_start) > (INFO_COLLECTION_TIMEOUT_MS / 1000.0)) {
                info_timeout = true;
                printf("Timeout waiting for device info from %s\n", dev->path);
            }
            usleep(100000); // 100ms sleep
        }
    } else {
        printf("Device %s is not MAVLink compatible (timeout)\n", dev->path);
    }

    cssl_close(dev->serial);
    cssl_stop();
    dev->thread_running = false;
    
    return NULL;
}

WEAK void start_mavlink_check(const char *devpath) {
    pthread_mutex_lock(&devices_mutex);
    
    if (device_count >= MAX_DEVICES) {
        fprintf(stderr, "Maximum device count reached, cannot monitor %s\n", devpath);
        pthread_mutex_unlock(&devices_mutex);
        return;
    }

    // Check if device is already being monitored
    for (int i = 0; i < device_count; i++) {
        if (strcmp(devices[i].path, devpath) == 0) {
            pthread_mutex_unlock(&devices_mutex);
            return;
        }
    }

    // Add new device
    strncpy(devices[device_count].path, devpath, DEV_PATH_LEN);
    devices[device_count].mavlink_valid = false;
    devices[device_count].thread_running = true;
    devices[device_count].serial = NULL;
    devices[device_count].id = device_count;
    devices[device_count].heartbeat_received = false;

    if (pthread_create(&devices[device_count].thread, NULL, check_mavlink_device, &devices[device_count]) != 0) {
        fprintf(stderr, "Failed to create thread for device %s\n", devpath);
        devices[device_count].thread_running = false;
    } else {
        printf("Started MAVLink check thread for %s (ID: %d)\n", devpath, device_count);
        device_count++;
    }

    pthread_mutex_unlock(&devices_mutex);
}

void print_device_info(const char *devname) {
    char devpath[DEV_PATH_LEN];
    snprintf(devpath, sizeof(devpath), "/dev/%s", devname);

    struct udev *udev;
    struct udev_device *dev;
    
    udev = udev_new();
    if (!udev) {
        fprintf(stderr, "Can't create udev\n");
        return;
    }
    
    dev = udev_device_new_from_subsystem_sysname(udev, "tty", devname);
    
    if (dev) {
        printf("Device Path: %s\n", devpath);
        printf("Device Name: %s\n", devname);
        
        struct udev_device *parent = udev_device_get_parent_with_subsystem_devtype(
            dev, "usb", "usb_device");
        if (parent) {
            printf("  VID/PID: %s %s\n",
                   udev_device_get_sysattr_value(parent, "idVendor"),
                   udev_device_get_sysattr_value(parent, "idProduct"));
            printf("  Manufacturer: %s\n",
                   udev_device_get_sysattr_value(parent, "manufacturer"));
            printf("  Product: %s\n",
                   udev_device_get_sysattr_value(parent, "product"));
            printf("  Serial: %s\n",
                   udev_device_get_sysattr_value(parent, "serial"));
        }
        #ifdef _DevCollecterAdvanced
        DeviceInfoTransport info = {
            .dev_path = devpath,
            .dev_name = devname,
            .vid = udev_device_get_sysattr_value(parent, "idVendor"),
            .pid = udev_device_get_sysattr_value(parent, "idProduct"),
            .manufacturer = udev_device_get_sysattr_value(parent, "manufacturer"),
            .product = udev_device_get_sysattr_value(parent, "product"),
            .serial = udev_device_get_sysattr_value(parent, "serial"),
            .usb_info_available = true
        };
        #endif
        
        udev_device_unref(dev);
    } else {
        printf("Device Path: %s\n", devpath);
        printf("Device Name: %s (no additional info available)\n", devname);
    }

    udev_unref(udev);
}

void scan_existing_devices(const DeviceTemplates *templates) {
    DIR *dir;
    struct dirent *ent;

    printf("\nScanning existing devices...\n");
    
    if ((dir = opendir("/dev")) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            if (is_monitored_device(ent->d_name, templates)) {
                char full_path[DEV_PATH_LEN];
                snprintf(full_path, sizeof(full_path), "/dev/%s", ent->d_name);
                
                printf("\nFound existing device: %s\n", full_path);
                print_device_info(ent->d_name);
                start_mavlink_check(full_path);
            }
        }
        closedir(dir);
    } else {
        perror("Could not open /dev directory");
    }
}

void print_usage(const char *program_name) {
    printf("Usage: %s <config.json> <ur-rpc-general-config.json> <ur-rpc-general-specific.json> \n", program_name);
}

void cleanup_threads() {
    pthread_mutex_lock(&devices_mutex);
    for (int i = 0; i < device_count; i++) {
        if (devices[i].thread_running) {
            pthread_cancel(devices[i].thread);
            pthread_join(devices[i].thread, NULL);
            if (devices[i].serial) {
                cssl_close(devices[i].serial);
            }
        }
    }
    pthread_mutex_unlock(&devices_mutex);
    cssl_stop();
}