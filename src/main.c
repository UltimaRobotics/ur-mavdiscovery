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

void on_message(struct mosquitto* mosq, void* userdata, const struct mosquitto_message* message) {
    MqttThreadContext* context_temp = (MqttThreadContext*)userdata;
    Config* config = &context_temp->config_base;
    pthread_mutex_lock(&context_temp->mutex);

    if (strcmp(message->topic, config->heartbeat_topic) == 0) {
        #ifdef _DEBUG_MODE
            printf("HeartBeat Message aquired \n");
        #endif
        cJSON* json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "process_id", config->process_id);
        cJSON_AddStringToObject(json, "response", "alive");
        char* response = cJSON_PrintUnformatted(json);
        mosquitto_publish(mosq, NULL, config->response_topic, strlen(response), response, 0, false);
        cJSON_Delete(json);
        free(response);
    }
    else if (strcmp(message->topic, config->module_update_topic) == 0) {
        cJSON* update_json = cJSON_Parse((char*)message->payload);
        if (update_json) {
            config_update update;
            memset(&update, 0, sizeof(update));
            
            cJSON* pid = cJSON_GetObjectItemCaseSensitive(update_json, "process_id");
            cJSON* type = cJSON_GetObjectItemCaseSensitive(update_json, "update_type");
            cJSON* value = cJSON_GetObjectItemCaseSensitive(update_json, "update_value");
            
            if (pid && type && value && 
                cJSON_IsString(pid) && 
                cJSON_IsNumber(type) && 
                cJSON_IsString(value)) {                
                if (strcmp(pid->valuestring, config->process_id) == 0) {
                    update.process_id = strdup(pid->valuestring);
                    update.update_type = (update_types)type->valueint;
                    update.update_value = strdup(value->valuestring);
                    
                    switch (update.update_type) {
                        case os_update:
                            #ifdef _DEBUG_MODE
                            printf("OS update requested: %s\n", update.update_value);
                            #endif
                            break;
                        case generic_config_update:
                            #ifdef _DEBUG_MODE
                            printf("Generic config update requested\n");
                            #endif
                            break; 
                        case target_specific_config_update: {
                            #ifdef _DEBUG_MODE
                            printf("Target-specific config update: %s\n", update.update_value);
                            #endif
                            cJSON* test_config = cJSON_Parse(update.update_value);
                            if (test_config) {
                                cJSON_Delete(test_config);
                                char backup_path[256];
                                snprintf(backup_path, sizeof(backup_path), "%s.bak", context_temp->config_paths.custom_config_path);
                                if (rename(context_temp->config_paths.custom_config_path, backup_path) != 0) {
                                    #ifdef _DEBUG_MODE
                                    fprintf(stderr, "Failed to create config backup\n");
                                    #endif
                                    break;
                                }
                                FILE* f = fopen(context_temp->config_paths.custom_config_path, "w");
                                if (f) {
                                    fprintf(f, "%s", update.update_value);
                                    fclose(f);
                                    #ifdef _DEBUG_MODE
                                    printf("Successfully updated target-specific configuration\n");    
                                    #endif                                
                                    free_custom_topics(&context_temp->config_additional);
                                    context_temp->config_additional = parse_custom_topics(context_temp->config_paths.custom_config_path);
                                } else {
                                    #ifdef _DEBUG_MODE
                                    fprintf(stderr, "Failed to write new config, restoring backup\n");
                                    #endif
                                    rename(backup_path, context_temp->config_paths.custom_config_path);
                                }
                            } else {
                                #ifdef _DEBUG_MODE
                                fprintf(stderr, "Invalid JSON configuration received\n");
                                #endif
                            }
                            break;
                        }
                    }
                    free(update.process_id);
                    free(update.update_value);
                }
            }
            cJSON_Delete(update_json);
        } else {
            #ifdef _DEBUG_MODE
            fprintf(stderr, "Failed to parse update message\n");
            #endif
        }
    }
    else {
        for (int i = 0; i < context_temp->config_additional.json_added_subs.topics_num; i++) {
            if (strcmp(message->topic, context_temp->config_additional.json_added_subs.topics[i]) == 0) {
                #ifdef _DEBUG_MODE
                printf("Received message on custom topic %s: %.*s\n",message->topic, message->payloadlen, (char*)message->payload);
                #endif
                break;
            }
        }
    }
    
    pthread_mutex_unlock(&context_temp->mutex);
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    
    DeviceTemplates templates;
    if (!load_templates_from_json(argv[1], &templates)) {
        return EXIT_FAILURE;
    }
    
    if (templates.count == 0) {
        #ifdef _DEBUG_MODE
        fprintf(stderr, "Error: No valid templates found in configuration file\n");
        #endif
        return EXIT_FAILURE;
    }
    #ifdef _DEBUG_MODE
    printf("Monitoring for devices matching these templates:\n");
        for (int i = 0; i < templates.count; i++) {
        printf("  %s\n", templates.templates[i]);
    }
    #endif

    
    // Scan existing devices first
    scan_existing_devices(&templates);
    #ifdef _DEBUG_MODE
    printf("\nStarting device monitoring...\n");
    #endif
    int fd, wd;
    char buffer[BUF_LEN];
    
    fd = inotify_init();
    if (fd < 0) {
        perror("inotify_init");
        cleanup_threads();
        return EXIT_FAILURE;
    }
    
    wd = inotify_add_watch(fd, "/dev", IN_CREATE | IN_DELETE);
    if (wd == -1) {
        perror("inotify_add_watch");
        close(fd);
        cleanup_threads();
        return EXIT_FAILURE;
    }

    context = malloc(sizeof(MqttThreadContext));
    memset(context, 0, sizeof(MqttThreadContext));
    pthread_mutex_init(&context->mutex, NULL);

    // Store config paths
    context->config_paths.base_config_path = strdup(argv[2]);
    context->config_paths.custom_config_path = strdup(argv[3]);

    // Load configurations
    context->config_base = parse_base_config(argv[2]);
    context->config_additional = parse_custom_topics(argv[3]);

    // Initialize monitors
    context->mqtt_monitor.last_activity = time(NULL);
    atomic_init(&context->mqtt_monitor.running, false);
    atomic_init(&context->mqtt_monitor.healthy, false);
    atomic_init(&context->health_monitor.running, false);

    // Set up thread attributes for detached threads
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    // Start MQTT thread
    if (pthread_create(&context->mqtt_monitor.thread_id, &attr, 
                      mqtt_thread_func, context) != 0) {
        fprintf(stderr, "Failed to create MQTT thread\n");
        goto cleanup;
    }

    // Start health monitor thread
    if (pthread_create(&context->health_monitor.thread_id, &attr, 
                      health_monitor_func, context) != 0) {
        fprintf(stderr, "Failed to create health monitor thread\n");
        atomic_store(&context->mqtt_monitor.running, false);
        goto cleanup;
    }

    pthread_attr_destroy(&attr);
    
    while (1) {
        int length = read(fd, buffer, BUF_LEN);
        if (length < 0) {
            perror("read");
            continue;
        }
        
        int i = 0;
        while (i < length) {
            struct inotify_event *event = (struct inotify_event *) &buffer[i];
            
            if (event->len && is_monitored_device(event->name, &templates)) {
                char full_path[DEV_PATH_LEN];
                snprintf(full_path, sizeof(full_path), "/dev/%s", event->name);
                
                if (event->mask & IN_CREATE) {
                    printf("\nDevice added at: %s\n", full_path);
                    print_device_info(event->name);
                    
                    start_mavlink_check(full_path);
                } else if (event->mask & IN_DELETE) {
                    printf("\nDevice removed from: %s\n", full_path);
                    unregister_device_mavrouter(full_path);
                }
            }
            
            i += EVENT_SIZE + event->len;
        }
    }
    cleanup:
    // Cleanup procedure
    atomic_store(&context->mqtt_monitor.running, false);
    atomic_store(&context->health_monitor.running, false);
    
    // Give threads time to exit
    sleep(1);
    
    // Free resources
    free_base_config(&context->config_base);
    free_custom_topics(&context->config_additional);
    free(context->config_paths.base_config_path);
    free(context->config_paths.custom_config_path);
    pthread_mutex_destroy(&context->mutex);
    free(context);
    
    inotify_rm_watch(fd, wd);
    close(fd);
    cleanup_threads();
    
    return EXIT_SUCCESS;
}