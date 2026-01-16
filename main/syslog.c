#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "syslog.h"
#include "settings.h"
#include "metrics.h"

static const char *TAG = "syslog";

// Syslog message queue
#define SYSLOG_QUEUE_SIZE 50
#define SYSLOG_MAX_MSG_LEN 1024

typedef struct {
    char *message;  // Dynamically allocated
    int priority;
} syslog_msg_t;

static QueueHandle_t syslog_queue = NULL;
static TaskHandle_t syslog_task_handle = NULL;
static settings_t *g_settings = NULL;
static int syslog_sock = -1;
static struct sockaddr_in syslog_addr;
static bool syslog_enabled = false;

// Syslog facility and severity constants
#define SYSLOG_FACILITY_USER 1
#define SYSLOG_SEVERITY_DEBUG 7
#define SYSLOG_SEVERITY_INFO 6
#define SYSLOG_SEVERITY_NOTICE 5
#define SYSLOG_SEVERITY_WARNING 4
#define SYSLOG_SEVERITY_ERROR 3
#define SYSLOG_SEVERITY_CRITICAL 2
#define SYSLOG_SEVERITY_ALERT 1
#define SYSLOG_SEVERITY_EMERGENCY 0

// Custom log handler for ESP-IDF logging system
static vprintf_like_t original_vprintf = NULL;

static int custom_vprintf(const char *fmt, va_list args) {
    // First, call the original vprintf to maintain console output
    int ret = 0;
    if (original_vprintf) {
        va_list args_copy;
        va_copy(args_copy, args);
        ret = original_vprintf(fmt, args_copy);
        va_end(args_copy);
    }

    // If syslog is enabled and we have a valid queue, send to syslog
    if (syslog_enabled && syslog_queue && fmt) {
        // Allocate message on heap
        char *message = malloc(SYSLOG_MAX_MSG_LEN);
        atomic_fetch_add(&malloc_count_syslog, 1);
        if (!message) {
            return ret;  // Skip if allocation fails
        }
        
        // Format the message
        vsnprintf(message, SYSLOG_MAX_MSG_LEN, fmt, args);
        
        // Parse log level from ESP-IDF log format
        // ESP-IDF logs typically start with a level indicator like "E (123) TAG: message"
        int severity = SYSLOG_SEVERITY_INFO;
        if (message[0] == 'E' && message[1] == ' ') {
            severity = SYSLOG_SEVERITY_ERROR;
        } else if (message[0] == 'W' && message[1] == ' ') {
            severity = SYSLOG_SEVERITY_WARNING;
        } else if (message[0] == 'I' && message[1] == ' ') {
            severity = SYSLOG_SEVERITY_INFO;
        } else if (message[0] == 'D' && message[1] == ' ') {
            severity = SYSLOG_SEVERITY_DEBUG;
        } else if (message[0] == 'V' && message[1] == ' ') {
            severity = SYSLOG_SEVERITY_DEBUG;
        }
        
        syslog_msg_t msg;
        msg.message = message;
        msg.priority = (SYSLOG_FACILITY_USER << 3) | severity;
        
        // Try to send to queue (non-blocking to avoid deadlocks)
        if (xQueueSend(syslog_queue, &msg, 0) != pdTRUE) {
            // Queue full, free the message
            free(message);
            atomic_fetch_add(&free_count_syslog, 1);
        }
    }

    return ret;
}

static void syslog_task(void *pvParameters) {
    syslog_msg_t msg;
    char syslog_packet[SYSLOG_MAX_MSG_LEN + 100];
    
    while (1) {
        // Wait for messages from the queue
        if (xQueueReceive(syslog_queue, &msg, portMAX_DELAY) == pdTRUE) {
            // Check if syslog is still enabled and configured
            if (!syslog_enabled || !g_settings || !g_settings->syslog_server || 
                strlen(g_settings->syslog_server) == 0) {
                free(msg.message);  // Free the message
                atomic_fetch_add(&free_count_syslog, 1);
                continue;
            }
            
            // Check if we need to create/recreate socket
            if (syslog_sock < 0) {
                syslog_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (syslog_sock < 0) {
                    // ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
                    free(msg.message);  // Free the message
                    atomic_fetch_add(&free_count_syslog, 1);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    continue;
                }
                
                // Resolve hostname
                struct hostent *he = gethostbyname(g_settings->syslog_server);
                if (he == NULL) {
                    // ESP_LOGE(TAG, "Failed to resolve hostname: %s", g_settings->syslog_server);
                    close(syslog_sock);
                    syslog_sock = -1;
                    free(msg.message);  // Free the message
                    atomic_fetch_add(&free_count_syslog, 1);
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    continue;
                }
                
                memset(&syslog_addr, 0, sizeof(syslog_addr));
                syslog_addr.sin_family = AF_INET;
                syslog_addr.sin_port = htons(g_settings->syslog_port);
                memcpy(&syslog_addr.sin_addr, he->h_addr_list[0], he->h_length);
            }
            
            // Get hostname for syslog message
            const char *hostname = g_settings->hostname ? g_settings->hostname : "esp32";
            
            // Format syslog message according to RFC 3164
            // <priority>timestamp hostname tag: message
            snprintf(syslog_packet, sizeof(syslog_packet),
                    "<%d>%s %s",
                    msg.priority,
                    hostname,
                    msg.message);
            
            // Send UDP packet
            int sent = sendto(syslog_sock, syslog_packet, strlen(syslog_packet), 0,
                            (struct sockaddr *)&syslog_addr, sizeof(syslog_addr));
            
            // Free the message after sending (or attempting to send)
            free(msg.message);
            atomic_fetch_add(&free_count_syslog, 1);
            
            if (sent < 0) {
                // ESP_LOGE(TAG, "Failed to send syslog message: errno %d", errno);
                // Close socket to force reconnection on next message
                close(syslog_sock);
                syslog_sock = -1;
            }
        }
    }
}

esp_err_t syslog_init(settings_t *settings) {
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }
    
    g_settings = settings;
    
    // Check if syslog is enabled and configured
    if (!settings->syslog_server || 
        strlen(settings->syslog_server) == 0) {
        ESP_LOGI(TAG, "Syslog is disabled or not configured");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing syslog client (server: %s:%d)", 
             settings->syslog_server, settings->syslog_port);
    
    // Create message queue
    syslog_queue = xQueueCreate(SYSLOG_QUEUE_SIZE, sizeof(syslog_msg_t));
    if (!syslog_queue) {
        ESP_LOGE(TAG, "Failed to create syslog queue");
        return ESP_ERR_NO_MEM;
    }
    
    // Create syslog task
    BaseType_t result = xTaskCreate(
        syslog_task,
        "syslog",
        4096,
        NULL,
        5,
        &syslog_task_handle
    );
    
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create syslog task");
        vQueueDelete(syslog_queue);
        syslog_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    // Hook into ESP-IDF logging system
    original_vprintf = esp_log_set_vprintf(custom_vprintf);
    syslog_enabled = true;
    
    ESP_LOGI(TAG, "Syslog client initialized successfully");
    
    return ESP_OK;
}

void syslog_deinit(void) {
    syslog_enabled = false;
    
    // Restore original vprintf
    if (original_vprintf) {
        esp_log_set_vprintf(original_vprintf);
        original_vprintf = NULL;
    }
    
    // Close socket
    if (syslog_sock >= 0) {
        close(syslog_sock);
        syslog_sock = -1;
    }
    
    // Delete task
    if (syslog_task_handle) {
        vTaskDelete(syslog_task_handle);
        syslog_task_handle = NULL;
    }
    
    // Delete queue and free any remaining messages
    if (syslog_queue) {
        syslog_msg_t msg;
        // Drain the queue and free any remaining messages
        while (xQueueReceive(syslog_queue, &msg, 0) == pdTRUE) {
            free(msg.message);
            atomic_fetch_add(&free_count_syslog, 1);
        }
        vQueueDelete(syslog_queue);
        syslog_queue = NULL;
    }
    
    g_settings = NULL;
    
    ESP_LOGI(TAG, "Syslog client deinitialized");
}

esp_err_t syslog_register(settings_t *settings, httpd_handle_t http_server) {
    // This function is a placeholder for potential HTTP configuration interface
    // Settings will be handled through the main settings module
    return ESP_OK;
}
