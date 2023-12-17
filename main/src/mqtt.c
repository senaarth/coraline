#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "cJSON.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"

#include "mqtt.h"
#include "oled.h"
#include "buzzer.h"

#define TAG "MQTT"

extern int DISPLAY_MODE;
extern int PLANT_STATUS;
extern float SOIL_MOISTURE;
extern float TEMPERATURE;
extern float HUMIDITY;
extern SemaphoreHandle_t conexaoMQTTSemaphore;

esp_mqtt_client_handle_t client;

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0)
    {
        ESP_LOGE(TAG, "Ultimo erro %s: 0x%x", message, error_code);
    }
}

int get_topico_id(const char *topico)
{
    // Encontrar a posição do último "/"
    const char *ultima_barra = strrchr(topico, '/');

    if (ultima_barra != NULL)
    {
        // Converter a parte do número para inteiro
        return atoi(ultima_barra + 1);
    }

    // Retorna -1 se não encontrar um número no final do tópico
    return -1;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Evento dispachado do loop base %s, ID %d", base, (int)event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        xSemaphoreGive(conexaoMQTTSemaphore);
        msg_id = esp_mqtt_client_subscribe(client, "v1/devices/me/rpc/request/+", 0);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        ESP_LOGI(TAG, "TOPICO %.*s\r", event->topic_len, event->topic);
        ESP_LOGI(TAG, "DATA %.*s\r", event->data_len, event->data);

        cJSON *root = cJSON_Parse(event->data);

        if (root != NULL)
        {
            cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");

            if (method != NULL && cJSON_IsString(method))
            {
                if (strcmp(method->valuestring, "change_display_mode") == 0)
                {
                    change_display_mode();
                }
                else if (strcmp(method->valuestring, "check_display_mode") == 0)
                {
                    char address[30];
                    char message[30];

                    int topico_id = get_topico_id(event->topic);

                    sprintf(address, "v1/devices/me/rpc/response/%d", topico_id);
                    sprintf(message, "{\"display_mode\":%d}", DISPLAY_MODE);

                    mqtt_envia_mensagem(address, message);
                }
                else if (strcmp(method->valuestring, "play_buzzer") == 0)
                {
                    play_buzzer();
                }
                else if (strcmp(method->valuestring, "stop_buzzer") == 0)
                {
                    // stop_buzzer();
                }
                else
                {
                    ESP_LOGE(TAG, "Evento RPC desconhecido: %s", method->valuestring);
                }
            }
            else
            {
                printf("Erro ao obter o método via JSON");
            }

            cJSON_Delete(root);
        }

        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno", event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "String do ultimo erro: (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
    default:
        ESP_LOGI(TAG, "ID de evento desconhecido:%d", event->event_id);
        break;
    }
}

void mqtt_start()
{
    esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = "",
        .credentials.username = ""};
    client = esp_mqtt_client_init(&mqtt_config);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
}

void mqtt_envia_mensagem(char *topico, char *mensagem)
{
    int message_id = esp_mqtt_client_publish(client, topico, mensagem, 0, 1, 0);
    ESP_LOGI(TAG, "Mensagem enviada, ID %d", message_id);
}

void comunicacao_servidor_task(void *params)
{
    char mensagem[200];
    char jsonAtributos[200];

    if (xSemaphoreTake(conexaoMQTTSemaphore, portMAX_DELAY))
    {
        while (true)
        {
            sprintf(mensagem, "{\"moisture\": %f, \"temperature\": %f, \"humidity\": %f, \n\"plant_status\": %d}", SOIL_MOISTURE, TEMPERATURE, HUMIDITY, PLANT_STATUS);
            mqtt_envia_mensagem("v1/devices/me/telemetry", mensagem);

            sprintf(jsonAtributos, "{\"display_mode\": %d, \n\"plant_status\": %d}", DISPLAY_MODE, PLANT_STATUS);
            mqtt_envia_mensagem("v1/devices/me/attributes", mensagem);

            vTaskDelay(3000 / portTICK_PERIOD_MS);
        }
    }
}
