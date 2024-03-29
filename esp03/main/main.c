/*
 *  ESP32 MQTT Thingsboard
 *  Copyright (C) 2022 Wellington Jonatan <wellpriz at gmail.com>
 *  This file is part of ESP32 MQTT Thingsboard.

 *  ESP32 MQTT Thingsboard is free software: you can redistribute it and/or
 *  modify it under the terms of the GNU Affero General Public License as
 *  published by the Free Software Foundation, either version 3 of the License,
 *  or (at your option) any later version.

 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.

 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "esp_sleep.h"
#include "driver/uart.h"

#include "dht11.h"

#include "wifi.h"
#include "http_client.h"
#include "mqtt.h"
#include "gpios.h"
#include "pwm.h"
#include "buzzer.h"

#define STR(x) #x
#define XSTR(x) STR(x)

#define TAG "MAIN"
#define GPIO_EVT_QUEUE_LEN (10)
#define ESP_INTR_FLAG_DEFAULT (0)

xSemaphoreHandle conexaoWifiSemaphore;
xSemaphoreHandle conexaoMQTTSemaphore;

static xQueueHandle gpio_evt_queue = NULL;

int temperature = 0;
int humidity = 0;
bool valid_dht11 = false;
bool valid_mqtt = false;

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
	uint32_t gpio_num = (uint32_t) arg;
	xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

void conectadoWifi(void *params)
{
	while (true) {
		if(xSemaphoreTake(conexaoWifiSemaphore, portMAX_DELAY)) {
			// Processamento Internet
			mqtt_start();
		}
	}
}

void trataComunicacaoComServidor(void *params)
{
	char mensagem[60];
	bool too_hot;
	bool too_cold;
	bool too_dry;
	bool too_wet;

	if (xSemaphoreTake(conexaoMQTTSemaphore, portMAX_DELAY)) {
		valid_mqtt = true;
		while (true) {
			if (valid_dht11) {
				too_hot = temperature > 25;
				too_cold = temperature < 20;
				too_dry = humidity < 40;
				too_wet = humidity > 70;

				sprintf(mensagem,
					"{\"temperature\": %d, \"humidity\": %d}",
					temperature, humidity);
				mqtt_envia_mensagem("v1/devices/me/telemetry",
					mensagem);

				sprintf(mensagem,
					"{\"too_hot\": %d, \"too_cold\": %d, \"too_wet\": %d, \"too_dry\": %d}",
					too_hot, too_cold, too_wet, too_dry);
				mqtt_envia_mensagem("v1/devices/me/attributes",
					mensagem);
			}

			vTaskDelay(3000 / portTICK_PERIOD_MS);
		}
	}
}

void readDHT11(void *params)
{
	while (true) {
		struct dht11_reading dht11_data = DHT11_read();
		if (dht11_data.status == DHT11_OK) {
			temperature = dht11_data.temperature;
			humidity = dht11_data.humidity;
			valid_dht11 = true;
			ESP_LOGI("DHT11", "Read successfully");
		} else {
			switch (dht11_data.status) {
			case DHT11_CRC_ERROR:
				ESP_LOGE("DHT11", "Read failed with status %d (CRC MISMATCH)", dht11_data.status);
				break;
			case DHT11_TIMEOUT_ERROR:
				ESP_LOGE("DHT11", "Read failed with status %d (TIMEOUT)", dht11_data.status);
				break;
			default:
				ESP_LOGE("DHT11", "Read failed with status %d", dht11_data.status);
			}
		}
		if (valid_dht11) {
			ESP_LOGI("DHT11", "Last temp: %d", temperature);
			ESP_LOGI("DHT11", "Last hum: %d", humidity);
		}
		vTaskDelay(4000 / portTICK_PERIOD_MS);
	}
}

void button_task(void *params)
{
	uint32_t gpio_num;
	int gpio_state;
	char msg[60];

	while (true) {
		if (xQueueReceive(gpio_evt_queue, &gpio_num, portMAX_DELAY)) {
			ESP_LOGI(TAG, "Got gpio %d", gpio_num);

			// Debounce
			gpio_state = gpio_get_level(gpio_num);

			if (valid_mqtt) {
				sprintf(msg, "{\"board_gpio\": %d}",
					gpio_state);
				mqtt_envia_mensagem("v1/devices/me/telemetry",
					msg);
			}

			gpio_isr_handler_remove(gpio_num);

			while (gpio_get_level(gpio_num) == gpio_state) {
				vTaskDelay(10 / portTICK_PERIOD_MS);
			}

			if (valid_mqtt) {
				gpio_state = gpio_get_level(gpio_num);
				sprintf(msg, "{\"board_gpio\": %d}",
					gpio_state);
				mqtt_envia_mensagem("v1/devices/me/telemetry",
					msg);
			}

			vTaskDelay(10 / portTICK_PERIOD_MS);
			gpio_isr_handler_add(gpio_num, gpio_isr_handler,
				(void *) gpio_num);
		}
	}
}

#ifndef CONFIG_BATTERY_MODE
void app_main(void)
{
	pwm_error_t pwm_error;

	ESP_LOGW(TAG, "ENERGY MODE");

	// Inicializa o NVS
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}

	ESP_ERROR_CHECK(ret);

	conexaoWifiSemaphore = xSemaphoreCreateBinary();
	conexaoMQTTSemaphore = xSemaphoreCreateBinary();

	DHT11_init(GPIO_DHT11);
	ESP_LOGI(TAG, "DHT 11 Initialised");

	pwm_error = pwm_init();

	if (pwm_error == PWM_OK)
		ESP_LOGI(TAG, "RGB LED PWM Initialised");

	pwm_error = buzzer_pwm_init();

	if (pwm_error == PWM_OK)
		ESP_LOGI(TAG, "BUZZER PWM Initialised");

	wifi_start();

	xTaskCreate(conectadoWifi, "Conexão ao MQTT", 4096, NULL, 1, NULL);
	xTaskCreate(trataComunicacaoComServidor, "Comunicação com Broker",
		4096, NULL, 1, NULL);
	xTaskCreate(readDHT11, "DHT11 reading", 4096, NULL, 3, NULL);

	gpio_reset_pin(GPIO_BOARD);

	// setup board button with internal pull-down
	gpio_reset_pin(GPIO_BOARD_BUTTON);
	gpio_set_direction(GPIO_BOARD_BUTTON, GPIO_MODE_INPUT);
	gpio_pullup_dis(GPIO_BOARD_BUTTON);
	gpio_pulldown_dis(GPIO_BOARD_BUTTON);
	gpio_pullup_en(GPIO_BOARD_BUTTON);

	// configure interruption
	gpio_set_intr_type(GPIO_BOARD_BUTTON, GPIO_INTR_ANYEDGE);

	gpio_evt_queue = xQueueCreate(GPIO_EVT_QUEUE_LEN, sizeof(uint32_t));

	xTaskCreate(button_task, "Button task", 4096, NULL, 2, NULL);

	gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
	gpio_isr_handler_add(GPIO_BOARD_BUTTON, gpio_isr_handler,
		(void *) GPIO_BOARD_BUTTON);
}
#else
void app_main()
{
	ESP_LOGW(TAG, "BATTERY MODE");

	pwm_error_t pwm_error;

	TaskHandle_t conectadoWifiHandle;
	TaskHandle_t trataComunicacaoComServidorHandle;
	TaskHandle_t readDHT11Handle;
	TaskHandle_t button_taskHandle;

	// Inicializa o NVS
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}

	ESP_ERROR_CHECK(ret);

	conexaoWifiSemaphore = xSemaphoreCreateBinary();
	conexaoMQTTSemaphore = xSemaphoreCreateBinary();

	DHT11_init(GPIO_DHT11);
	ESP_LOGI(TAG, "DHT 11 Initialised");

	pwm_error = pwm_init();

	if (pwm_error == PWM_OK)
		ESP_LOGI(TAG, "RGB LED PWM Initialised");

	pwm_error = buzzer_pwm_init();

	if (pwm_error == PWM_OK)
		ESP_LOGI(TAG, "BUZZER PWM Initialised");

	wifi_start();

	xTaskCreate(conectadoWifi, "Conexão ao MQTT", 4096, NULL, 1, &conectadoWifiHandle);
	xTaskCreate(trataComunicacaoComServidor, "Comunicação com Broker",
		4096, NULL, 1, &trataComunicacaoComServidorHandle);
	xTaskCreate(readDHT11, "DHT11 reading", 4096, NULL, 3, &readDHT11Handle);

	gpio_reset_pin(GPIO_BOARD);

	// setup board button with internal pull-down
	gpio_reset_pin(GPIO_BOARD_BUTTON);
	gpio_set_direction(GPIO_BOARD_BUTTON, GPIO_MODE_INPUT);
	gpio_pullup_dis(GPIO_BOARD_BUTTON);
	gpio_pulldown_dis(GPIO_BOARD_BUTTON);
	gpio_pullup_en(GPIO_BOARD_BUTTON);

	// configure interruption
	gpio_set_intr_type(GPIO_BOARD_BUTTON, GPIO_INTR_ANYEDGE);

	gpio_evt_queue = xQueueCreate(GPIO_EVT_QUEUE_LEN, sizeof(uint32_t));

	xTaskCreate(button_task, "Button task", 4096, NULL, 2, &button_taskHandle);

	gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
	gpio_isr_handler_add(GPIO_BOARD_BUTTON, gpio_isr_handler,
		(void *) GPIO_BOARD_BUTTON);
	gpio_wakeup_enable(GPIO_BOARD_BUTTON, GPIO_INTR_LOW_LEVEL);
	esp_sleep_enable_gpio_wakeup();

	while (true) {
		vTaskDelay(10000 / portTICK_PERIOD_MS);

		ESP_LOGW(TAG, "Suspending tasks");
		gpio_isr_handler_remove(GPIO_BOARD_BUTTON);
		vTaskSuspend(conectadoWifiHandle);
		vTaskSuspend(trataComunicacaoComServidorHandle);
		vTaskSuspend(readDHT11Handle);
		vTaskSuspend(button_taskHandle);

		ESP_LOGW(TAG, "Begin light sleep");
		uart_wait_tx_idle_polling(CONFIG_ESP_CONSOLE_UART_NUM);
		esp_light_sleep_start();

		ESP_LOGW(TAG, "Resuming tasks");
		gpio_isr_handler_add(GPIO_BOARD_BUTTON, gpio_isr_handler,
			(void *) GPIO_BOARD_BUTTON);
		vTaskResume(conectadoWifiHandle);
		vTaskResume(trataComunicacaoComServidorHandle);
		vTaskResume(readDHT11Handle);
		vTaskResume(button_taskHandle);

		ESP_LOGW(TAG, "Woke up");
		vTaskDelay(10000 / portTICK_PERIOD_MS);
	}
}
#endif
