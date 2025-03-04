#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_vfs.h"
#include "QMI8658.h"

#define FILE_PATH "/spiffs/sensor_data.csv"

// I2C Configuration
#define I2C_MASTER_SCL_IO    7
#define I2C_MASTER_SDA_IO    6
#define I2C_MASTER_NUM       I2C_NUM_0
#define I2C_MASTER_FREQ_HZ   400000

// Log tag
static const char *TAG = "QMI8658";

// SPIFFS Initialization
void init_spiffs() {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true  // Format SPIFFS if mounting fails
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SPIFFS mounted successfully");

        // Check SPIFFS storage info
        size_t total = 0, used = 0;
        esp_spiffs_info(NULL, &total, &used);
        ESP_LOGI(TAG, "SPIFFS Total: %d bytes, Used: %d bytes", total, used);
    }
}




// void init_spiffs() {
//     ESP_LOGI(TAG, "Initializing SPIFFS...");

//     esp_vfs_spiffs_conf_t conf = {
//         .base_path = "/spiffs",
//         .partition_label = NULL,
//         .max_files = 5,
//         .format_if_mount_failed = true
//     };

//     esp_err_t ret = esp_vfs_spiffs_register(&conf);
//     if (ret != ESP_OK) {
//         ESP_LOGE(TAG, "SPIFFS mount failed (%s)", esp_err_to_name(ret));
//     } else {
//         ESP_LOGI(TAG, "SPIFFS mounted successfully");
//     }
// }

// // Function to write sensor data to a CSV file
// void write_to_csv(float acc[3], float gyro[3]) {


//     struct stat st;
//     if (stat(FILE_PATH, &st) == 0) {
//     ESP_LOGI(TAG, "File exists! Size: %ld bytes", st.st_size);
//     } else {
//     ESP_LOGW(TAG, "File does not exist. Creating a new one...");
//     }


//     FILE *file = fopen(FILE_PATH, "a");
//     if (file == NULL) {
//         ESP_LOGE(TAG, "Failed to open file for writing");
//         return;
//     }

//     // Get timestamp (millis since boot)
//     uint32_t timestamp = esp_log_timestamp();

//     // Default activity is "None"
//     fprintf(file, "%lu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,None\n",
//             timestamp, acc[0], acc[1], acc[2], gyro[0], gyro[1], gyro[2]);

//     fclose(file);
//     ESP_LOGI(TAG, "Data written to CSV");
// }

// // Sensor Task: Reads accelerometer & gyroscope values and writes to CSV
// void sensor_task(void *pvParameters) {
//     while (1) {
//         float acc[3], gyro[3];

//         // Read sensor data
//         QMI8658_read_xyz(acc, gyro, NULL);

//         // Log data
//         //ESP_LOGI(TAG, "Accelerometer (mg): X=%.2f Y=%.2f Z=%.2f", acc[0], acc[1], acc[2]);
//         //ESP_LOGI(TAG, "Gyroscope (dps): X=%.2f Y=%.2f Z=%.2f", gyro[0], gyro[1], gyro[2]);

//         // Write data to CSV
//         write_to_csv(acc, gyro);

//         // Delay (10ms) for 100Hz
//         vTaskDelay(pdMS_TO_TICKS(10));
//     }
// }


// /**
// * New approach for file write
// */

// FILE *filenew = NULL;

// void sensor_task(void *pvParameters) {
//     filenew = fopen(FILE_PATH, "a"); // Open once at task start
//     if (!filenew) {
//         ESP_LOGE(TAG, "Failed to open file");
//         vTaskDelete(NULL);
//     }
//     float acc[3], gyro[3];
//     while (1) {
//         // Read sensor data
//         QMI8658_read_xyz(acc, gyro, NULL);

//         // Write data to CSV
//         fprintf(filenew, "%lu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,None\n",
//                 esp_log_timestamp(), acc[0], acc[1], acc[2], gyro[0], gyro[1], gyro[2]);
        
//         // Flush periodically (every 10 samples)
//         static int count = 0;
//         if (++count >= 5) {
//             fflush(filenew);
//             count = 0;
//         }

//         vTaskDelay(pdMS_TO_TICKS(20)); // 50Hz
//     }
//     fclose(filenew); // Close when task ends
// }

/**
 * Producer- Consumer approach
 */
#include "freertos/queue.h"
#include "freertos/semphr.h"

#define BUFFER_SIZE 4096     // Store 100 samples (1 second of data at 100Hz)
#define FLUSH_THRESHOLD 50    // Flush when buffer has 50 samples

 typedef struct {
    uint32_t timestamp;
    float acc[3];
    float gyro[3];
} SensorData;

// Ring buffer and synchronization
static SensorData buffer[BUFFER_SIZE];
static volatile uint16_t head = 0;  // Write index
static volatile uint16_t tail = 0;  // Read index
static SemaphoreHandle_t buffer_mutex;

#define TEMP_BUFFER_SIZE 200   // 60ms write time so need to adjust acccordingly(min threshold)

static SensorData temp_buffer[TEMP_BUFFER_SIZE];  // Temporary writer buffer
static volatile uint8_t temp_index = 0;          // Write index for temp buffer

void sensor_task(void *pvParameters) {
    float acc[3], gyro[3];

    while (1) {
        int64_t start_time = esp_timer_get_time();

        // Read sensor data
        QMI8658_read_xyz(acc, gyro, NULL);

        // Store in temporary buffer first
        temp_buffer[temp_index].timestamp = esp_log_timestamp();
        memcpy(temp_buffer[temp_index].acc, acc, sizeof(acc));
        memcpy(temp_buffer[temp_index].gyro, gyro, sizeof(gyro));
        temp_index++;

        // Flush to ring buffer when temp buffer is full
        if (temp_index >= TEMP_BUFFER_SIZE) {
            if (xSemaphoreTake(buffer_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                for (uint8_t i = 0; i < TEMP_BUFFER_SIZE; i++) {
                    buffer[head] = temp_buffer[i];  // Copy data
                    uint16_t next_head = (head + 1) % BUFFER_SIZE;

                    // Handle overflow
                    if (next_head == tail) {
                        tail = (tail + 1) % BUFFER_SIZE;
                        ESP_LOGW(TAG, "Buffer overflow!");
                    }
                    head = next_head;
                }
                xSemaphoreGive(buffer_mutex);
                temp_index = 0;  // Reset temp buffer index
            } else {
                ESP_LOGW(TAG, "Failed to acquire mutex, temp buffer not flushed!");
            }
        }

        // Strict 20ms delay for 50Hz
        vTaskDelay(pdMS_TO_TICKS(20));

        int64_t end_time = esp_timer_get_time();
        ESP_LOGI(TAG, "Time taken for sensor task is %f ms", (double)(end_time - start_time) / 1000.0);
    }
}

void writer_task(void *pvParameters) {
    FILE *file = fopen(FILE_PATH, "a");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file");
        vTaskDelete(NULL);
    }

    SensorData *local_buffer = (SensorData *)malloc(FLUSH_THRESHOLD * sizeof(SensorData));
    if (!local_buffer) {
        ESP_LOGE(TAG, "Failed to allocate memory");
        fclose(file);
        vTaskDelete(NULL);
    }

    while (1) {
        int64_t start_time = esp_timer_get_time();

        vTaskDelay(pdMS_TO_TICKS(100));  // Process data periodically

        uint16_t num_to_write = 0;

        // Acquire mutex only briefly for fast copy
        if (xSemaphoreTake(buffer_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            num_to_write = (head >= tail) ? (head - tail) : (BUFFER_SIZE - tail + head);
            num_to_write = (num_to_write > FLUSH_THRESHOLD) ? FLUSH_THRESHOLD : num_to_write;

            for (uint16_t i = 0; i < num_to_write; i++) {
                uint16_t idx = (tail + i) % BUFFER_SIZE;
                local_buffer[i] = buffer[idx];  // Copy data
            }

            tail = (tail + num_to_write) % BUFFER_SIZE;  // Update tail
            xSemaphoreGive(buffer_mutex);  // Release mutex quickly
        }

        // Write to file outside critical section
        if (num_to_write > 0) {
            for (uint16_t i = 0; i < num_to_write; i++) {
                fprintf(file, "%lu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
                        local_buffer[i].timestamp,
                        local_buffer[i].acc[0], local_buffer[i].acc[1], local_buffer[i].acc[2],
                        local_buffer[i].gyro[0], local_buffer[i].gyro[1], local_buffer[i].gyro[2]);
            }
            fflush(file);
            ESP_LOGI(TAG, "Wrote %d samples to file", num_to_write);
        }

        int64_t end_time = esp_timer_get_time();
        ESP_LOGI(TAG, "Time taken for writer task is %f ms", (double)(end_time - start_time) / 1000.0);
    }

    free(local_buffer);
    fclose(file);
    vTaskDelete(NULL);
}



// /**
//  * FreeRTOS queue approach
//  */

 
//  #define QUEUE_SIZE 200  // Increase queue size to handle burst data
//  #define BATCH_SIZE 50    // Write 50 samples at once to minimize delays
 
//  typedef struct {
//      uint32_t timestamp;
//      float acc[3];
//      float gyro[3];
//  } SensorData;
 
//  static QueueHandle_t sensor_queue;  // FreeRTOS queue
 
//  // Producer Task (Sensor Data Collection)
//  void sensor_task(void *pvParameters) {
//      SensorData data;
 
//      while (1) {
//          // Read sensor data
//          float acc[3], gyro[3];
//          QMI8658_read_xyz(acc, gyro, NULL);
 
//          data.timestamp = esp_log_timestamp();
//          memcpy(data.acc, acc, sizeof(acc));
//          memcpy(data.gyro, gyro, sizeof(gyro));
 
//          // Try sending data to queue (non-blocking, to avoid blocking `sensor_task`)
//          BaseType_t xHigherPriorityTaskWoken = pdFALSE;
//          if (xQueueSendFromISR(sensor_queue, &data, &xHigherPriorityTaskWoken) == errQUEUE_FULL) {
//              ESP_LOGW(TAG, "Queue full, data lost!");
//          }
 
//          vTaskDelay(pdMS_TO_TICKS(20));  // Strict 50hz sampling
//      }
//  }
 
//  // Consumer Task (Batch File Writing)
//  void writer_task(void *pvParameters) {
//      FILE *file = fopen(FILE_PATH, "a");
//      if (!file) {
//          ESP_LOGE(TAG, "Failed to open file");
//          vTaskDelete(NULL);
//      }
 
//      SensorData local_buffer[BATCH_SIZE];
 
//      while (1) {
//          uint16_t num_to_write = 0;
 
//          // Collect BATCH_SIZE samples
//          while (num_to_write < BATCH_SIZE) {
//              if (xQueueReceive(sensor_queue, &local_buffer[num_to_write], pdMS_TO_TICKS(5)) == pdTRUE) {
//                  num_to_write++;
//              } else {
//                  break;  // Exit if no more data
//              }
//          }
 
//          // Write batch to file (non-blocking)
//          if (num_to_write > 0) {
//              for (uint16_t i = 0; i < num_to_write; i++) {
//                  fprintf(file, "%lu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
//                          local_buffer[i].timestamp,
//                          local_buffer[i].acc[0], local_buffer[i].acc[1], local_buffer[i].acc[2],
//                          local_buffer[i].gyro[0], local_buffer[i].gyro[1], local_buffer[i].gyro[2]);
//              }
//              fflush(file);
//              ESP_LOGI(TAG, "Wrote %d samples to file", num_to_write);
//          }
//      }
 
//      fclose(file);
//      vTaskDelete(NULL);
//  }


//Initialize the csv file
void csvColumn(){
    FILE *file = fopen(FILE_PATH, "a");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        return;
    }
    fprintf(file, "timestamp,acc_X,acc_Y,acc_Z,gyro_X,gyro_Y,gyro_Z,activity \n");
    fclose(file);
}

/**
 * To download the csv file
 */
#include <esp_http_server.h>
#include <esp_vfs.h>
#include <esp_vfs_fat.h>
#include <esp_spiffs.h>
#include <sys/stat.h>

esp_err_t file_get_handler(httpd_req_t *req) {
    FILE *file = fopen(FILE_PATH, "r");
    if (!file) {
        ESP_LOGE("HTTP", "Failed to open file for reading");
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    char buf[512];  // Buffer to read file content
    size_t read_bytes;
    httpd_resp_set_type(req, "text/csv"); // Set Content-Type for CSV file

    while ((read_bytes = fread(buf, 1, sizeof(buf), file)) > 0) {
        httpd_resp_send_chunk(req, buf, read_bytes);
    }
    fclose(file);
    httpd_resp_send_chunk(req, NULL, 0); // End response

    return ESP_OK;
}

void start_http_server() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t file_download = {
            .uri       = "/download", 
            .method    = HTTP_GET,
            .handler   = file_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &file_download);
        ESP_LOGI("HTTP", "HTTP Server started! Access: http:// &ip_info.ip /download");
    }
}


/**
 * For getting the IP address and connecting with WiFi
 */
// #include "esp_wifi.h"
// #include "esp_event.h"
// #include "esp_netif.h"
// #include "nvs_flash.h"

// #define WIFI_SSID      "BIRAJ-PC"
// #define WIFI_PASS      "birajpaul"

// #define STATIC_IP      "192.168.0.1"
// #define GATEWAY_IP     "192.168.0.1"
// #define NETMASK        "255.255.255.0"

// static void wifi_event_handler(void* arg, esp_event_base_t event_base,
//                                int32_t event_id, void* event_data) {
//     if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
//         esp_wifi_connect();
//     } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
//         ESP_LOGI("WiFi", "Disconnected, reconnecting...");
//         esp_wifi_connect();
//     }
// }

// void wifi_init() {
//     ESP_ERROR_CHECK(nvs_flash_init());
//     ESP_ERROR_CHECK(esp_netif_init());
//     ESP_ERROR_CHECK(esp_event_loop_create_default());

//     esp_netif_t *netif = esp_netif_create_default_wifi_sta(); // Create Wi-Fi station interface
//     wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
//     ESP_ERROR_CHECK(esp_wifi_init(&cfg));

//     esp_event_handler_instance_t instance_any_id;
//     ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
//                                                         ESP_EVENT_ANY_ID,
//                                                         &wifi_event_handler,
//                                                         NULL,
//                                                         &instance_any_id));

//     wifi_config_t wifi_config = {
//         .sta = {
//             .ssid = WIFI_SSID,
//             .password = WIFI_PASS,
//             .scan_method = WIFI_FAST_SCAN,
//         },
//     };
//     ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
//     ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
//     ESP_ERROR_CHECK(esp_wifi_start());

//     ESP_LOGI("WiFi", "Wi-Fi initialized, setting static IP...");

//     // Convert string IPs to esp_ip4_addr_t
//     esp_netif_ip_info_t ip_info;
//     ESP_ERROR_CHECK(esp_netif_str_to_ip4(STATIC_IP, &ip_info.ip));
//     ESP_ERROR_CHECK(esp_netif_str_to_ip4(GATEWAY_IP, &ip_info.gw));
//     ESP_ERROR_CHECK(esp_netif_str_to_ip4(NETMASK, &ip_info.netmask));

//     // Disable DHCP and apply static IP
//     ESP_ERROR_CHECK(esp_netif_dhcpc_stop(netif));
//     ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip_info));

//     ESP_LOGI("WiFi", "Static IP set to: %s", STATIC_IP);
// }

 

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#define WIFI_SSID      "BIRAJ-PC"
#define WIFI_PASS      "birajpaul"

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI("WiFi", "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI("WiFi", "Disconnected, reconnecting...");
        esp_wifi_connect();
    }
}

void wifi_init() {
    ESP_ERROR_CHECK(nvs_flash_init());  // Initialize NVS
    ESP_ERROR_CHECK(esp_netif_init());  // Initialize network interface
    ESP_ERROR_CHECK(esp_event_loop_create_default()); // Create event loop

    esp_netif_create_default_wifi_sta(); // Create Wi-Fi station interface
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); // Set as Wi-Fi client
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI("WiFi", "Wi-Fi initialized, connecting...");
}


//for deleting the file
void deleteFile(){
    const char *file_path = "/spiffs/sensor_data.csv"; 

if (unlink(file_path) == 0) {
    ESP_LOGI(TAG, "File deleted successfully: %s", file_path);
} else {
    ESP_LOGE(TAG, "Failed to delete file: %s", file_path);
}
}

// Initialize and Start Everything
extern "C" void app_main(void) {
    // Initialize networking (prevents tcpip assertion failure)
    wifi_init();
    // Initialize SPIFFS
    init_spiffs();

    // Initialize I2C for sensor
    i2c_config_t conf;
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = I2C_MASTER_SDA_IO;
    conf.scl_io_num = I2C_MASTER_SCL_IO;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = I2C_MASTER_FREQ_HZ;

    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);

    // Initialize sensor
    if (QMI8658_init()) {
        ESP_LOGI(TAG, "Sensor initialized successfully");
    } else {
        ESP_LOGE(TAG, "Sensor initialization failed");
        return;
    }

    // Initialize SPIFFS
    // esp_vfs_spiffs_conf_t conf = {
    //     .base_path = "/spiffs",
    //     .partition_label = NULL,
    //     .max_files = 5,
    //     .format_if_mount_failed = true
    // };
    //ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));

    deleteFile();

    csvColumn();
    // FILE *file = fopen("/spiffs/sensor_data.csv", "r");
    // if (!file) {
    //     ESP_LOGE("HTTP", "File not found!");
    // }

    // Start HTTP Server
    start_http_server();

    // Create FreeRTOS task to read sensor data
    // xTaskCreate(sensor_task, "sensor_task", 8196, NULL, 5, NULL);
    
    // Initialize buffer synchronization
    buffer_mutex = xSemaphoreCreateMutex();
    if (!buffer_mutex) {
        ESP_LOGE(TAG, "Failed to create buffer mutex");
        return;
    }
    // Create tasks
    xTaskCreate(sensor_task, "Sensor Task", 4096, NULL, 10, NULL);
    xTaskCreate(writer_task, "Writer Task", 8192, NULL, 5, NULL);

    // // Create queue (200 sensor samples max)
    // sensor_queue = xQueueCreate(QUEUE_SIZE, sizeof(SensorData));
    // if (!sensor_queue) {
    //     ESP_LOGE(TAG, "Failed to create queue");
    //     return;
    // }

    // // Create tasks (Sensor on Core 1, Writer on Core 0)
    // xTaskCreatePinnedToCore(sensor_task, "Sensor Task", 4096, NULL, 3, NULL, 1);  // LOW priority
    // xTaskCreatePinnedToCore(writer_task, "Writer Task", 8192, NULL, 5, NULL, 0);  // HIGHOW priority
}