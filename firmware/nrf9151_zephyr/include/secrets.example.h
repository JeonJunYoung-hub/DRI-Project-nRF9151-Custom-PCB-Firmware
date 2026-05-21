#ifndef SECRETS_EXAMPLE_H
#define SECRETS_EXAMPLE_H

/*
 * Copy this file to secrets.h for private local builds.
 * Keep the real secrets.h file out of Git.
 */

#define SENSOR_ID       "devices/example-device/telemetry"
#define MQTT_LOG        "devices/example-device/logs"

#define MQTT_BROKER     "mqtt.example.com"
#define MQTT_PORT       1883

#define PINNUMBER       ""
#define APN             "your.apn.example"
#define APN_USER        "your-apn-user"
#define APN_PASSWORD    "your-apn-password"

#endif
