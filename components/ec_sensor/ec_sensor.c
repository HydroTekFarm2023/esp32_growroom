/*
 * ec_sensor.c
 *
 *  Created on: May 1, 2020
 *      Author: ajayv
 */

#include "ec_sensor.h"
#include <esp_log.h>
#include <esp_err.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <math.h>

#define DEFAULT_VREF    1100
#define MULTIPLIER      1000000
#define RES2 			820.0
#define ECREF 			200.0

static const char *TAG = "DF_Robot_EC_Sensor";

static float  _KValue = 1.0;
static float  _KValueLow = 1.0;
static float  _KValueHigh = 1.0;
static float  _voltage = 0;
static float  _temperature = 0;
static float  _rawEC = 0;

esp_err_t ec_begin(){
	esp_err_t error;
	nvs_handle_t my_handle;
	error = nvs_open("storage", NVS_READWRITE, &my_handle);
	if(error != ESP_OK){
		return error;
	}

	int32_t TempKValueLow = MULTIPLIER;
	error = nvs_get_i32(my_handle, "KValueLow", &TempKValueLow);
	if(error == ESP_ERR_NVS_NOT_FOUND){
		nvs_set_i32(my_handle, "KValueLow", TempKValueLow);
		nvs_commit(my_handle);
		ESP_LOGI(TAG, "KValueLow set to 1. Calibration must take place");
	} else if (error != ESP_OK){
		return error;
	}

	int32_t TempKValueHigh = MULTIPLIER;
	error = nvs_get_i32(my_handle, "KValueHigh", &TempKValueHigh);
	if(error == ESP_ERR_NVS_NOT_FOUND){
		nvs_set_i32(my_handle, "KValueHigh", TempKValueHigh);
		nvs_commit(my_handle);
		ESP_LOGI(TAG, "KValueHigh set to 1. Calibration must take place");
	} else if (error != ESP_OK){
		return error;
	}
	nvs_close(my_handle);

	_KValueLow = (float) TempKValueLow / (float) MULTIPLIER;
	_KValueHigh = (float) TempKValueHigh / (float) MULTIPLIER;
	_KValue = _KValueLow;
	ESP_LOGI(TAG, "KValueLow = %f  TempKValueLow = %d \n", _KValueLow, TempKValueLow);
	ESP_LOGI(TAG, "KValueHigh = %f  TempKValueHigh = %d \n", _KValueHigh, TempKValueHigh);
	return ESP_OK;
}

float readEC(float voltage, float temperature){
	float adjustedEC = 0;
	float valueTemp = 0;

	_rawEC = 1000*voltage/RES2/ECREF;
	valueTemp = _rawEC * _KValue;
	if(valueTemp > 2.5){
		_KValue = _KValueHigh;
		ESP_LOGI(TAG, "KValueHigh: %f\n", _KValueHigh);
	}else if(valueTemp < 2.0){
		_KValue = _KValueLow;
		ESP_LOGI(TAG, "KValueLow: %f\n", _KValueLow);
	}
	adjustedEC = (_rawEC * _KValue) / (1.0+0.0185*(temperature-25.0));

	_voltage = voltage;
	_temperature = temperature;
	return adjustedEC;
}

esp_err_t calibrateEC(){
	static float compECsolution;
	float KValueTemp;

	if(_voltage == 0 && _rawEC == 0 && _temperature == 0){
		return false;
	}

	if((_rawEC>0.9)&&(_rawEC<1.9)){
		compECsolution = 1.413*(1.0+0.0185*(_temperature-25.0));
		ESP_LOGI(TAG, "1.413us/cm buffer solution\n");
	}else if((_rawEC>9)&&(_rawEC<16.8)){
		compECsolution = 12.88*(1.0+0.0185*(_temperature-25.0));
		ESP_LOGI(TAG, "12.88ms/cm buffer solution\n");
	} else {
		ESP_LOGE(TAG, "error: rawEC not in range\n");
		return false;
	}

	KValueTemp = RES2*ECREF*compECsolution/1000.0/_voltage;

	if((KValueTemp>0.5) && (KValueTemp<1.5)){
		esp_err_t error;
		nvs_handle_t my_handle;
		error = nvs_open("storage", NVS_READWRITE, &my_handle);
		if(error != ESP_OK){
			return error;
		}
		if((_rawEC>0.9)&&(_rawEC<1.9)){
			KValueTemp = round(KValueTemp * MULTIPLIER);
			_KValueLow =  KValueTemp / MULTIPLIER;
			error = nvs_set_i32(my_handle, "KValueLow", KValueTemp);
			nvs_commit(my_handle);
			if(error != ESP_OK){
				ESP_LOGD(TAG, "error: %d", error);
			}
			ESP_LOGI(TAG, "Calibration success! KValueLow: %f   KValueTemp: %f\n", _KValueLow, KValueTemp);
		}else if((_rawEC>9)&&(_rawEC<16.8)){
			KValueTemp = round(KValueTemp * MULTIPLIER);
			_KValueHigh =  KValueTemp / MULTIPLIER;
			nvs_set_i32(my_handle, "KValueHigh", KValueTemp);
			nvs_commit(my_handle);
			ESP_LOGI(TAG, "Calibration success! KValueHigh: %f   KValueTemp: %f\n", _KValueHigh, KValueTemp);

		}
		nvs_close(my_handle);
	} else {
		ESP_LOGE(TAG, "KValueTemp failed as not in range: %f\n", KValueTemp);
		return false;
	}
	return ESP_OK;
}





