#ifndef _MQTTCONNECTION_H_
#define _MQTTCONNECTION_H_
#define LED_CONTROL_OVER_MQTT          DK_LED1 /*The LED to control over MQTT*/
#define IMEI_LEN 15
#define CGSN_RESPONSE_LENGTH (IMEI_LEN + 6 + 1) /* Add 6 for \r\nOK\r\n and 1 for \0 */
#define CLIENT_ID_LEN sizeof("nrf-") + IMEI_LEN

#pragma once
#include <stdbool.h>

extern volatile bool mqtt_connected;

int publish_sensor_data(const char *device_id_str, float pm1, float pm25, float pm4, float pm10,
                        float rh, float temp, float co2, float voltage, float hPa, uint32_t result_raw_NOx,uint32_t result_Raw_VOC, uint64_t now_ms);

int publish_sensor_data_B(const char *device_id_str, float pm1, float pm25, float pm4, float pm10,
                        float rh, float temp, float co2, float voltage,float hPa, uint32_t result_raw_NOx, uint32_t result_Raw_VOC, uint64_t now_ms);

int Publish_sensor_data_Gnss_pressure(const char *device_id_str, uint64_t now_ms);
						
/**@brief Initialize the MQTT client structure
 */
int client_init(struct mqtt_client *client);

/**@brief Initialize the file descriptor structure used by poll.
 */
int fds_init(struct mqtt_client *c, struct pollfd *fds);

/**@brief Function to publish data on the configured topic
 */
int data_publish(struct mqtt_client *c, enum mqtt_qos qos,
	uint8_t *data, size_t len, char *topic);

#endif /* _CONNECTION_H_ */
