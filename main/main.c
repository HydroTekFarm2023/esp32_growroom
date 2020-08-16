#include <inttypes.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_event.h>
#include <esp_event_loop.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_err.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <mqtt_client.h>
#include <lwip/sockets.h>
#include <lwip/dns.h>
#include <lwip/netdb.h>
#include <stdbool.h>

#include "bme680.h"
#include "ds3231.h"
#include "rf_transmission.h"

// WiFi Coordination with Event Group
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT (1<<0)
#define WIFI_FAIL_BIT      (1<<1)

// Sensor Task Coordination with Event Group
static EventGroupHandle_t sensor_event_group;
#define DELAY_BIT		    (1<<0)
#define BME_BIT           (1<<5)

// Core 0 Task Priorities
#define TIMER_ALARM_TASK_PRIORITY 0
#define MQTT_PUBLISH_TASK_PRIORITY 1
#define SENSOR_CONTROL_TASK_PRIORITY 2

// Core 1 Task Priorities
#define BME_TASK_PRIORITY 0
#define SYNC_TASK_PRIORITY 1

// GPIO Ports
#define I2C_SDA_GPIO 21                 // GPIO 21
#define I2C_SCL_GPIO 22                 // GPIO 22
#define RF_TRANSMITTER_GPIO 32			// GPIO 32

#define SENSOR_MEASUREMENT_PERIOD 10000 // Measuring increment time in ms
#define SENSOR_DISABLED_PERIOD SENSOR_MEASUREMENT_PERIOD / 2 // Disabled increment is half of measure period so task always finishes on time

#define RETRYMAX 5 // WiFi MAX Reconnection Attempts

static int retryNumber = 0;  // WiFi Reconnection Attempts

// IDs
static char growroom_id[] = "growroom1";
static char system_id[] = "system2";

// Sensor Measurement Variables
static float _air_temp = 0;
static float _humidity = 0;

// Temp control
static bool changing_temp = false;
static float target_temp = 25;
static float temp_desired_margin_error = 1.5;
static float temp_optimal_margin_error = 0.75;
static bool temp_checks[6] = {false, false, false, false, false, false};

// Humidity control
static bool changing_humidity = false;
static float target_humidity = 40;
static float humidity_desired_margin_error = 5;
static float humidity_optimal_margin_error = 2.5;
static bool humidity_checks[6] = {false, false, false, false, false, false};

// RTC Components
i2c_dev_t dev;

// Timer and alarm periods
static const uint32_t timer_alarm_urgent_delay = 10;
static const uint32_t timer_alarm_regular_delay = 50;

// Timers

// Alarms

// Task Handles
static TaskHandle_t bme_task_handle = NULL;
static TaskHandle_t sync_task_handle = NULL;
static TaskHandle_t timer_alarm_task_handle = NULL;
static TaskHandle_t publish_task_handle = NULL;
static TaskHandle_t sensor_control_task_handle = NULL;

// Sensor Active Status
static bool bme_active = true;

static uint32_t sensor_sync_bits;

static void restart_esp32() { // Restart ESP32
	fflush(stdout);
	esp_restart();
}

static void init_rtc() { // Init RTC
	memset(&dev, 0, sizeof(i2c_dev_t));
	ESP_ERROR_CHECK(ds3231_init_desc(&dev, 0, I2C_SDA_GPIO, I2C_SCL_GPIO));
}
static void set_time() { // Set current time to some date
	// TODO Have user input for time so actual time is set
	struct tm time;

	time.tm_year = 120; // Years since 1900
	time.tm_mon = 6; // 0-11
	time.tm_mday = 7; // day of month
	time.tm_hour = 9; // 0-24
	time.tm_min = 59;
	time.tm_sec = 0;

	ESP_ERROR_CHECK(ds3231_set_time(&dev, &time));
}

static void check_rtc_reset() {
	// Get current time
	struct tm time;
	ds3231_get_time(&dev, &time);

	// If year is less than 2020 (RTC was reset), set time again
	if(time.tm_year < 120) set_time();
}

static void get_date_time(struct tm *time) {
	// Get current time and set it to return var
	ds3231_get_time(&dev, &(*time));

	// If year is less than 2020 (RTC was reset), set time again and set new time to return var
	if(time->tm_year < 120) {
		set_time();
		ds3231_get_time(&dev, &(*time));
	}
}

static void event_handler(void *arg, esp_event_base_t event_base,		// WiFi Event Handler
		int32_t event_id, void *event_data) {
	const char *TAG = "Event_Handler";
	ESP_LOGI(TAG, "Event dispatched from event loop base=%s, event_id=%d\n",
			event_base, event_id);

	// Check Event Type
	if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t *event = (ip_event_got_ip_t*) event_data;
		ESP_LOGI(TAG, "got IP:%s", ip4addr_ntoa(&event->ip_info.ip));
		retryNumber = 0;
		xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		ESP_ERROR_CHECK(esp_wifi_connect());
		retryNumber = 0;
	} else if (event_base == WIFI_EVENT
			&& event_id == WIFI_EVENT_STA_DISCONNECTED) {
		// Attempt Reconnection
		if (retryNumber < RETRYMAX) {
			esp_wifi_connect();
			retryNumber++;
		} else {
			xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
		}
		ESP_LOGI(TAG, "WIFI Connection Failed; Reconnecting....\n");
	}
}

static void mqtt_event_handler(void *arg, esp_event_base_t event_base,		// MQTT Event Callback Functions
								int32_t event_id, void *event_data) {
	const char *TAG = "MQTT_Event_Handler";
	switch (event_id) {
	case MQTT_EVENT_CONNECTED:
		xTaskNotifyGive(publish_task_handle);
		ESP_LOGI(TAG, "Connected\n");
		break;
	case MQTT_EVENT_DISCONNECTED:
		ESP_LOGI(TAG, "Disconnected\n");
		break;
	case MQTT_EVENT_SUBSCRIBED:
		ESP_LOGI(TAG, "Subscribed\n");
		break;
	case MQTT_EVENT_UNSUBSCRIBED:
		ESP_LOGI(TAG, "UnSubscribed\n");
		break;
	case MQTT_EVENT_PUBLISHED:
		ESP_LOGI(TAG, "Published\n");
		break;
	case MQTT_EVENT_DATA:
//		if (true) {
//			ec_calibration = true;
//		}
		break;
	case MQTT_EVENT_ERROR:
		ESP_LOGI(TAG, "Error\n");
		break;
	case MQTT_EVENT_BEFORE_CONNECT:
		ESP_LOGI(TAG, "Before Connection\n");
		break;
	default:
		ESP_LOGI(TAG, "Other Command\n");
		break;
	}
}

void create_str(char** str, char* init_str) { // Create method to allocate memory and assign initial value to string
	*str = malloc(strlen(init_str) * sizeof(char)); // Assign memory based on size of initial value
	if(!(*str)) { // Restart if memory alloc fails
		ESP_LOGE("", "Memory allocation failed. Restarting ESP32");
		restart_esp32();
	}
	strcpy(*str, init_str); // Copy initial value into string
}
void append_str(char** str, char* str_to_add) { // Create method to reallocate and append string to already allocated string
	*str = realloc(*str, (strlen(*str) + strlen(str_to_add)) * sizeof(char) + 1); // Reallocate data based on existing and new string size
	if(!(*str)) { // Restart if memory alloc fails
		ESP_LOGE("", "Memory allocation failed. Restarting ESP32");
		restart_esp32();
	}
	strcat(*str, str_to_add); // Concatenate strings
}

// Add sensor data to JSON entry
void add_entry(char** data, bool* first, char* name, float num) {
	// Add a comma to the beginning of every entry except the first
	if(*first) *first = false;
	else append_str(data, ",");

	// Convert float data into string
	char value[8];
	snprintf(value, sizeof(value), "%.2f", num);

	// Create entry string
	char *entry = NULL;
	create_str(&entry, "{ name: \"");

	// Create entry using key, value, and other JSON syntax
	append_str(&entry, name);
	append_str(&entry, "\", value: \"");
	append_str(&entry, value);
	append_str(&entry, "\"}");

	// Add entry to overall JSON data
	append_str(data, entry);

	// Free allocated memory
	free(entry);
	entry = NULL;
}

void publish_data(void *parameter) {			// MQTT Setup and Data Publishing Task
	const char *TAG = "Publisher";

//	// Set broker configuration
//	esp_mqtt_client_config_t mqtt_cfg = { .host = "70.94.9.135", .port = 1883 };
//
//	// Create and initialize MQTT client
//	esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
//	esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler,
//			client);
//	esp_mqtt_client_start(client);
//	ulTaskNotifyTake( pdTRUE, portMAX_DELAY);

	for (;;) {
		// Create and structure topic for publishing data through MQTT
		char *topic = NULL;
		create_str(&topic, growroom_id);
		append_str(&topic, "/");
		append_str(&topic, "systems/");
		append_str(&topic, system_id);

		// Create and initially assign JSON data
		char *data = NULL;
		create_str(&data, "{ \"time\": ");

		// Add timestamp to data
		struct tm time;
		get_date_time(&time);

		// Convert all time componenets to string
		uint32_t year_int = time.tm_year + 1900;
		char year[8];
		snprintf(year, sizeof(year), "%.4d", year_int);

		uint32_t month_int = time.tm_mon + 1;
		char month[8];
		snprintf(month, sizeof(month), "%.2d", month_int);

		char day[8];
		snprintf(day, sizeof(day), "%.2d", time.tm_mday);

		char hour[8];
		snprintf(hour, sizeof(hour), "%.2d", time.tm_hour);

		char min[8];
		snprintf(min, sizeof(min), "%.2d", time.tm_min);

		char sec[8];
		snprintf(sec, sizeof(sec), "%.2d", time.tm_sec);

		// Format timestamp in standard ISO format (https://www.w3.org/TR/NOTE-datetime)
		char *date = NULL;;
		create_str(&date, "\"");
		append_str(&date, year);
		append_str(&date, "-");
		append_str(&date, month);
		append_str(&date, "-");
		append_str(&date,  day);
		append_str(&date, "T");
		append_str(&date, hour);
		append_str(&date, "-");
		append_str(&date, min);
		append_str(&date, "-");
		append_str(&date, sec);
		append_str(&date, "Z\", sensors: [");

		// Append formatted timestamp to data
		append_str(&data, date);
		free(date);
		date = NULL;

		// Variable for adding comma to every entry except first
		bool first = true;

		// Check if all the sensors are active and add data to JSON string if so using corresponding key and value
		if(bme_active) {
			add_entry(&data, &first, "air temp", _air_temp);
			add_entry(&data, &first, "humidity", _humidity);
		}

		// Add closing tag
		append_str(&data, "]}");

		ESP_LOGI(TAG, "Topic: %s", topic);
		ESP_LOGI(TAG, "Message: %s", data);

//		// Publish data to MQTT broker using topic and data
//		esp_mqtt_client_publish(client, topic, data, 0, 1, 0);

		// Free allocated memory
		free(data);
		data = NULL;

		// Publish data every 20 seconds
		vTaskDelay(pdMS_TO_TICKS(SENSOR_MEASUREMENT_PERIOD));
	}
}

void reset_sensor_checks(bool *sensor_checks) { // Reset sensor check vars
	for(int i = 0; i < sizeof(sensor_checks); i++) {
		sensor_checks[i] = false;
	}
}

void check_temperature() {
	char *TAG = "TEMP_CONTROL";

	// Check if temp is being currently changed
	if(changing_temp) {
		// If temp is good now, turn off heater and cooler
		if(_air_temp > target_temp - temp_optimal_margin_error && _air_temp < target_temp + temp_optimal_margin_error) {
			// TODO turn off cooling and heating mechanisms
			ESP_LOGI(TAG, "Temperature control done");
			changing_temp = false;
		}
	//  Temp is not being currently changed
	} else {
		// Check if temp is too low
		if(_air_temp < target_temp - temp_desired_margin_error) {
			// If checks are done, turn on heater and reset checks
			if(temp_checks[sizeof(temp_checks) - 1])  {
				// TODO turn on heating mechanism
				ESP_LOGI(TAG, "Heating room");
				reset_sensor_checks(temp_checks);
				changing_temp = true;
			// Otherwise, iterate through checks and set next one to true
			} else {
				for(int i = 0; i < sizeof(temp_checks); i++) {
					if(!temp_checks[i]) {
						temp_checks[i] = true;
						ESP_LOGI(TAG, "Temp check %d done", i + 1);
						break;
					}
				}
			}
		// Check if temp is too high
		} else if(_air_temp > target_temp + temp_desired_margin_error)  {
			// If checks are done, turn on cooler and reset checks
			if(temp_checks[sizeof(temp_checks) - 1])  {
				// TODO turn on cooling mechanism
				ESP_LOGI(TAG, "Cooling room");
				reset_sensor_checks(temp_checks);
				changing_temp = true;
			// Otherwise, iterate through checks and set next one to true
			} else {
				for(int i = 0; i < sizeof(temp_checks); i++) {
					if(!temp_checks[i]) {
						temp_checks[i] = true;
						ESP_LOGI(TAG, "Temp check %d done", i + 1);
						break;
					}
				}
			}
		// If temp is fine, just reset checks
		} else {
			reset_sensor_checks(temp_checks);
		}
	}
}

void check_humidity() {
	char *TAG = "HUMIDITY_CONTROL";

	// Check if humidity is being currently changed
	if(changing_humidity) {
		// If humidity is good now, turn off humidifier and dehumidifer
		if(_humidity > target_humidity - humidity_optimal_margin_error && _humidity < target_humidity + humidity_optimal_margin_error) {
			// TODO turn off humidifier and dehumidifier mechanisms
			ESP_LOGI(TAG, "Humidity control done");
			changing_humidity = false;
		}
	//  Humidity is not being currently changed
	} else {
		// Check if humidity is too low
		if(_humidity < target_humidity - humidity_desired_margin_error) {
			// If checks are done, turn on humidifier and reset checks
			if(humidity_checks[sizeof(humidity_checks) - 1])  {
				// TODO turn on humidifier mechanism
				ESP_LOGI(TAG, "Humidifying room");
				reset_sensor_checks(humidity_checks);
				changing_humidity = true;
			// Otherwise, iterate through checks and set next one to true
			} else {
				for(int i = 0; i < sizeof(humidity_checks); i++) {
					if(!humidity_checks[i]) {
						humidity_checks[i] = true;
						ESP_LOGI(TAG, "Humidity check %d done", i + 1);
						break;
					}
				}
			}
		// Check if humidity is too high
		} else if(_humidity > target_humidity + humidity_desired_margin_error)  {
			// If checks are done, turn on dehumidifier and reset checks
			if(humidity_checks[sizeof(humidity_checks) - 1])  {
				// TODO turn on dehumidifier mechanism
				ESP_LOGI(TAG, "Dehumidifying room");
				reset_sensor_checks(humidity_checks);
				changing_humidity = true;
			// Otherwise, iterate through checks and set next one to true
			} else {
				for(int i = 0; i < sizeof(humidity_checks); i++) {
					if(!humidity_checks[i]) {
						humidity_checks[i] = true;
						ESP_LOGI(TAG, "Humidity check %d done", i + 1);
						break;
					}
				}
			}
		// If humidity is fine, just reset checks
		} else {
			reset_sensor_checks(humidity_checks);
		}
	}
}

void sensor_control(void * parameter) { // Sensor control task
	for(;;) {
		// Check sensors
		//check_temperature();
		check_humidity();

		// Wait till next sensor readings
		vTaskDelay(pdMS_TO_TICKS(SENSOR_MEASUREMENT_PERIOD));
	}
}

void measure_bme(void * parameter) {
	const char *TAG = "BME";

	bme680_t sensor;
	memset(&sensor, 0, sizeof(bme680_t));

	ESP_ERROR_CHECK(bme680_init_desc(&sensor, BME680_I2C_ADDR_1, 0, I2C_SDA_GPIO, I2C_SCL_GPIO));

	// Init the sensor
	ESP_ERROR_CHECK(bme680_init_sensor(&sensor));

	// Changes the oversampling rates to 4x oversampling for temperature
	// and 2x oversampling for humidity. Pressure measurement is skipped.
	bme680_set_oversampling_rates(&sensor, BME680_OSR_4X, BME680_OSR_NONE, BME680_OSR_2X);

	// Change the IIR filter size for temperature and pressure to 7.
	bme680_set_filter_size(&sensor, BME680_IIR_SIZE_7);

	// Set ambient temperature
	bme680_set_ambient_temperature(&sensor, 22);

	// As long as sensor configuration isn't changed, duration is constant
	uint32_t duration;
	bme680_get_measurement_duration(&sensor, &duration);

	bme680_values_float_t values;
	for(;;) {
		// trigger the sensor to start one TPHG measurement cycle
		if (bme680_force_measurement(&sensor) == ESP_OK) {
			// passive waiting until measurement results are available
			vTaskDelay(duration);
			// get the results and do something with them
			if (bme680_get_results_float(&sensor, &values) == ESP_OK) {
				ESP_LOGI(TAG, "Temperature: %.2f", values.temperature);
				ESP_LOGI(TAG, "Humidity: %.2f", values.humidity);

				_air_temp = values.temperature;
				_humidity = values.humidity;
			}
		}

		// Sync with other sensor tasks
		// Wait up to 10 seconds to let other tasks end
		xEventGroupSync(sensor_event_group, BME_BIT, sensor_sync_bits, pdMS_TO_TICKS(SENSOR_MEASUREMENT_PERIOD));
	}
}

void set_sensor_sync_bits(uint32_t *bits) {
	//*bits = 24;
	*bits = DELAY_BIT | (bme_active  ? BME_BIT : 0);
}

void sync_task(void *parameter) {				// Sensor Synchronization Task
	const char *TAG = "Sync_Task";
	EventBits_t returnBits;
	for (;;) {
		// Provide a minimum amount of time before event group round resets
		vTaskDelay(pdMS_TO_TICKS(10000));
		returnBits = xEventGroupSync(sensor_event_group, DELAY_BIT, sensor_sync_bits, pdMS_TO_TICKS(10000));
		// Check Whether all tasks were completed on time
		if ((returnBits & sensor_sync_bits) == sensor_sync_bits) {
			ESP_LOGI(TAG, "Completed");
		} else {
			ESP_LOGE(TAG, "Failed to Complete On Time");
		}
	}
}

static void manage_timers_alarms(void *parameter) {
	const char *TAG = "TIMER_TASK";

	// Initialize timers

	// Initialize alarms

	ESP_LOGI(TAG, "Timers initialized");

	for(;;) {
		// Get current unix time
		time_t unix_time;
		get_unix_time(&dev, &unix_time);

		// Check if timers are done

		// Check if alarms are done

		// Check if any timer or alarm is urgent
		bool urgent = false;

		// Set priority and delay based on urgency of timers and alarms
		vTaskPrioritySet(timer_alarm_task_handle, urgent ? (configMAX_PRIORITIES - 1) : TIMER_ALARM_TASK_PRIORITY);
		vTaskDelay(pdMS_TO_TICKS(urgent ? timer_alarm_urgent_delay : timer_alarm_regular_delay));
	}
}

void send_rf_transmission(){
	// Setup Transmission Protcol
	struct binary_bits low_bit = {3, 1};
	struct binary_bits sync_bit = {31, 1};
	struct binary_bits high_bit = {1, 3};
	configure_protocol(172, 10, 32, low_bit, high_bit, sync_bit);

	// Start controlling outlets
	send_message("000101000101010100110011"); // Binary Code to turn on Power Outlet 1
	vTaskDelay(pdMS_TO_TICKS(5000));
	send_message("000101000101010100111100"); // Binary Code to turn off Power Outlet 1
	vTaskDelay(pdMS_TO_TICKS(5000));
}

void app_main(void) {							// Main Method
	const char *TAG = "MAIN";

	// Check if space available in NVS, if not reset NVS
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES
			|| ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	// Initialize TCP IP stack and create WiFi management event loop
	tcpip_adapter_init();
	esp_event_loop_create_default();
	wifi_event_group = xEventGroupCreate();

	// Initialize WiFi and configure WiFi connection settings
	const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	// TODO: Update to esp_event_handler_instance_register()
	esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
	esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL);
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	wifi_config_t wifi_config = { .sta = {
			.ssid = "LeawoodGuest",
			.password = "guest,123" },
	};
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());

	// Do not proceed until WiFi is connected
	EventBits_t eventBits;
	eventBits = xEventGroupWaitBits(wifi_event_group,
	WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
	portMAX_DELAY);

	if ((eventBits & WIFI_CONNECTED_BIT) != 0) {
		// Init i2cdev
		ESP_ERROR_CHECK(i2cdev_init());

		// Init rtc and check if time needs to be set
		init_rtc();
		check_rtc_reset();

		sensor_event_group = xEventGroupCreate();
		set_sensor_sync_bits(&sensor_sync_bits);

		// Create core 0 tasks
		xTaskCreatePinnedToCore(manage_timers_alarms, "timer_alarm_task", 2500, NULL, TIMER_ALARM_TASK_PRIORITY, &timer_alarm_task_handle, 0);
		xTaskCreatePinnedToCore(publish_data, "publish_task", 2500, NULL, MQTT_PUBLISH_TASK_PRIORITY, &publish_task_handle, 0);
		xTaskCreatePinnedToCore(sensor_control, "sensor_control_task", 2500, NULL, SENSOR_CONTROL_TASK_PRIORITY, &sensor_control_task_handle, 0);

		// Create core 1 tasks
		if(bme_active) xTaskCreatePinnedToCore(measure_bme, "bme_task", 2500, NULL, BME_TASK_PRIORITY, &bme_task_handle, 1);
		xTaskCreatePinnedToCore(sync_task, "sync_task", 2500, NULL, SYNC_TASK_PRIORITY, &sync_task_handle, 1);

	} else if ((eventBits & WIFI_FAIL_BIT) != 0) {
		ESP_LOGE(TAG, "WIFI Connection Failed\n");
	} else {
		ESP_LOGE(TAG, "Unexpected Event\n");
	}
}
