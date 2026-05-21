#ifndef SEN55_H
#define SEN55_H

#include <zephyr/device.h>
#include <zephyr/IOT_PIPELINEvers/i2c.h>
#include <stdint.h>


#define SEN55_I2C_ADDR 0x69
#define SEN55_DATA_LEN 48


struct sen55_data {
    float pm1p0;
    float pm2p5;
    float pm4p0;
    float pm10p0;
    float temperature;
    float humidity;
    float voc_index;
    float nox_index;
};

int sen55_start_measurement(const struct device *i2c_dev);
int sen55_read_raw_data(const struct device *i2c_dev, uint8_t *buf, size_t len);
int sen55_parse_data(const uint8_t *buf, struct sen55_data *out);



#endif