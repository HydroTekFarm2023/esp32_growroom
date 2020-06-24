/*
 * rf_transmission.h
 *
 *  Created on: Jun 12, 2020
 *      Author: ajayv
 */

#ifndef COMPONENTS_RF_TRANSMISSION_RF_TRANSMISSION_H_
#define COMPONENTS_RF_TRANSMISSION_RF_TRANSMISSION_H_

#include <freertos/FreeRTOS.h>

struct binary_bits{
	uint32_t low_pulse_amount;
	uint32_t high_pulse_amount;
};

struct protocol {
	uint32_t pulse_width;
	struct binary_bits low_bit;
	struct binary_bits high_bit;
	struct binary_bits sync_bit;
	uint32_t repeat_transmission;
	uint16_t transmit_pin;
} power_outlet_protocol;

void configure_protocol(int32_t pulse_width, int32_t repeat_transmission, int16_t transmit_pin, struct binary_bits low_bit, struct binary_bits high_bit, struct binary_bits sync_bit);

/**
 * @brief Configure the Transmission Protocol
 * 		  Should be called before send_message function
 *
 * @param pulse_width	Pulse Width of particular RF Protocol
 * @param repeat_transmission Number of times to send binary message (Great repeats ensures higher accuracy)
 * @param transmit_pin	GPIO port of the Transmitter's Data Pin
 * @param low_bit	High Pulse and Low Pulse amount for sending binary 0
 * @param high_bit	High Pulse and Low Pulse amount for sending binary 1
 * @param sync_bit	High Pulse and Low Pulse amount for sending binary sync code
 *
 * @returns void
 */

void send_message(const char* binary_code);

/**
 * @brief Send binary message to power outlet
 *
 * @param binary_code	specific binary message to transmit
 * 						message should consist of address and action
 *
 * @returns void
 */

#endif /* COMPONENTS_RF_TRANSMISSION_RF_TRANSMISSION_H_ */
