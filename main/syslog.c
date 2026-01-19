#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
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
    char message[SYSLOG_MAX_MSG_LEN];
    int priority;
} syslog_msg_t;

static QueueHandle_t syslog_queue = NULL;
static TaskHandle_t syslog_task_handle = NULL;
static settings_t *g_settings = NULL;
static int syslog_sock = -1;
static struct sockaddr_in syslog_addr;
static bool syslog_enabled = false;

static syslog_msg_t send_msg;
// These are heap allocated in syslog_task
static syslog_msg_t *recv_msg = NULL;
static char *syslog_packet = NULL;


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
static SemaphoreHandle_t vprintf_mutex = NULL;

static int custom_vprintf(const char *fmt, va_list args) {
    // Take mutex with timeout to prevent deadlocks
    if (vprintf_mutex && xSemaphoreTake(vprintf_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        // If we can't get the mutex, just return to avoid blocking
        return 0;
    }
    
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
        
        // Format the message
        vsnprintf(send_msg.message, SYSLOG_MAX_MSG_LEN, fmt, args);
        
        // Parse log level from ESP-IDF log format
        // ESP-IDF logs typically start with a level indicator like "E (123) TAG: message"
        int severity = SYSLOG_SEVERITY_INFO;
        if (send_msg.message[0] == 'E' && send_msg.message[1] == ' ') {
            severity = SYSLOG_SEVERITY_ERROR;
        } else if (send_msg.message[0] == 'W' && send_msg.message[1] == ' ') {
            severity = SYSLOG_SEVERITY_WARNING;
        } else if (send_msg.message[0] == 'I' && send_msg.message[1] == ' ') {
            severity = SYSLOG_SEVERITY_INFO;
        } else if (send_msg.message[0] == 'D' && send_msg.message[1] == ' ') {
            severity = SYSLOG_SEVERITY_DEBUG;
        } else if (send_msg.message[0] == 'V' && send_msg.message[1] == ' ') {
            severity = SYSLOG_SEVERITY_DEBUG;
        }
        send_msg.priority = (SYSLOG_FACILITY_USER << 3) | severity;
        
        // Try to send to queue (non-blocking to avoid deadlocks)
        xQueueSend(syslog_queue, &send_msg, 0);
    }

    if (vprintf_mutex) {
        xSemaphoreGive(vprintf_mutex);
    }
    
    return ret;
}

static void syslog_task(void *pvParameters) {
    while (1) {
        // Wait for messages from the queue
        if (xQueueReceive(syslog_queue, recv_msg, portMAX_DELAY) == pdTRUE) {
            // Check if syslog is still enabled and configured
            if (!syslog_enabled || !g_settings || !g_settings->syslog_server || 
                strlen(g_settings->syslog_server) == 0) {
                continue;
            }
            
            // Check if we need to create/recreate socket
            if (syslog_sock < 0) {
                syslog_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (syslog_sock < 0) {
                    // ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    continue;
                }
                
                // Resolve hostname
                struct hostent *he = gethostbyname(g_settings->syslog_server);
                if (he == NULL) {
                    // ESP_LOGE(TAG, "Failed to resolve hostname: %s", g_settings->syslog_server);
                    close(syslog_sock);
                    syslog_sock = -1;

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
            snprintf(syslog_packet, SYSLOG_MAX_MSG_LEN + 100,
                    "<%d>%s %s",
                    recv_msg->priority,
                    hostname,
                    recv_msg->message);
            
            // Send UDP packet
            int sent = sendto(syslog_sock, syslog_packet, strlen(syslog_packet), 0,
                            (struct sockaddr *)&syslog_addr, sizeof(syslog_addr));
            
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
    
    // Create mutex for vprintf
    if (!vprintf_mutex) {
        vprintf_mutex = xSemaphoreCreateMutex();
        if (!vprintf_mutex) {
            ESP_LOGE(TAG, "Failed to create vprintf mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    recv_msg = malloc(sizeof(syslog_msg_t));
    syslog_packet = malloc(SYSLOG_MAX_MSG_LEN + 100);
    atomic_fetch_add(&malloc_count_syslog, 2);
    
    
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
    
    // Delete mutex
    if (vprintf_mutex) {
        vSemaphoreDelete(vprintf_mutex);
        vprintf_mutex = NULL;
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
    if (syslog_queue != NULL) {
        // Drain the queue and free any remaining messages
        vQueueDelete(syslog_queue);
        syslog_queue = NULL;
    }
    
    g_settings = NULL;
    if (recv_msg != NULL) {
        free(recv_msg);
        atomic_fetch_add(&free_count_syslog, 1);
        recv_msg = NULL;
    }
    if (syslog_packet != NULL) {
        free(syslog_packet);
        atomic_fetch_add(&free_count_syslog, 1);
        syslog_packet = NULL;
    }
    
    ESP_LOGI(TAG, "Syslog client deinitialized");
}

esp_err_t syslog_register(settings_t *settings, httpd_handle_t http_server) {
    // This function is a placeholder for potential HTTP configuration interface
    // Settings will be handled through the main settings module
    return ESP_OK;
}
