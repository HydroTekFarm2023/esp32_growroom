/*
 * ec_sensor.h
 *
 *  Created on: May 1, 2020
 *      Author: ajayv
 */

#ifndef PH_SENSOR_H
#define PH_SENSOR_H

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ph_begin();

/**
 * @brief Read KValueHigh and KValueLow from Non Volatile Storage
 *
 * If sensor has not been calibrated previously then a default KValueHigh and KValueLow of 1
 * will be written to the Non Volatile Storage
 *
 * This should be called before any other ec_sensor function
 *
 * @returns 'ESP_OK' if data from Non Volatile Storage was successfully read or written
 * 			It will return any other esp_err_t if read or write failed
 */

float readPH(float voltage);

esp_err_t calibratePH();

#ifdef __cplusplus
}
#endif

#endif /* PH_SENSOR_H */


