/*
 * ec_sensor.h
 *
 *  Created on: May 1, 2020
 *      Author: ajayv
 */

#ifndef EC_SENSOR_H
#define EC_SENSOR_H

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ec_begin();

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

float readEC(float voltage, float temperature);

/**
 * @brief Calculate EC from raw voltage adjusted for temperature
 *
 * @param voltage		Raw Voltage in mV measured from ADC
 * @param temperature	Temperature in Celsius measured from a temperature sensor such as DS18B20
 */

esp_err_t calibrateEC();

/**
 * @brief Perform Two Point Calibration and store new KValueHigh and KValueLow
 * 		  into Non Volatile Storage
 *
 * Calibration must be done twice with 1.413us/cm and 12.88ms/cm
 *
 * This must be called after readEC(). It will return an error if readEC isn't called first as
 * voltage and temperature measurements will not be present to perform calibration.
 *
 * Wait for the voltage and temperature values to remain relatively stable before starting
 * calibration process to get best results.
 *
 * @returns 'ESP_OK' if calibration process was successful and KValueHigh and KValueLow was
 * 			stored into Non Volatile Storage. It will return any other esp_err_t if calibration
 * 			was unsuccessful.
 */

#ifdef __cplusplus
}
#endif

#endif /* EC_SENSOR_H_ */


