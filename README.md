# Embedded LTE MQTT IoT Pipeline

A portfolio-safe embedded-to-cloud example for collecting environmental sensor data with a Nordic nRF9151-class LTE/GNSS device and forwarding telemetry through MQTT into MongoDB.

This repository intentionally uses placeholder broker, APN, topic, and database values. It is meant to show the architecture and implementation approach without exposing production infrastructure or private research code.

## Architecture

```text
SEN66/BMP581 sensors -> nRF9151 firmware -> LTE-M/NB-IoT -> MQTT broker -> Python bridge -> MongoDB
```

## Repository Layout

- `firmware/nrf9151_zephyr/` - Zephyr/Nordic firmware example for LTE, GNSS, sensor sampling, MQTT publishing, flash buffering, RTC scheduling, and watchdog recovery.
- `backend/mqtt_mongodb_bridge/` - Python MQTT subscriber that normalizes telemetry and stores it in MongoDB.
- `docs/` - high-level setup notes for the cloud and device pipeline.
- `images/` - architecture visuals.

## Features

- LTE-M/NB-IoT connectivity workflow using Nordic modem libraries
- GNSS location acquisition before telemetry upload
- MQTT telemetry publishing with configurable topics
- SEN66 and BMP581 sensor integration over I2C
- Adaptive sampling intervals based on PM2.5 levels
- Flash buffering for recent sensor and location payloads
- Watchdog and RTC-based scheduling for field reliability
- MongoDB ingestion service with environment-based configuration

## Privacy and Safety Notes

The public version removes:

- real MongoDB credentials and cluster names
- real MQTT broker IP addresses
- personal names and organization-specific identifiers
- build outputs containing local machine paths
- private topic names and internal device labels

Before using this in a real deployment, create private local config files from the examples and keep them out of Git.

## Firmware Setup

Install Nordic Connect SDK and build from the firmware directory:

```powershell
cd firmware/nrf9151_zephyr
west build -b nrf9151dk/nrf9151/ns -- -DCONF_FILE=prj.conf
```

Update `prj.conf` and a private `include/secrets.h` with your own MQTT broker, APN, and topics before deploying to hardware.

## Backend Setup

```powershell
cd backend/mqtt_mongodb_bridge
pip install -r requirements.txt
$env:MONGODB_URI="<your MongoDB connection string>"
$env:MQTT_HOST="localhost"
python bridge.py
```

## Example MQTT Topics

- `devices/example-device/telemetry`
- `devices/example-device/location`
- `devices/example-device/logs`

## Status

This repository is a sanitized reference implementation. Hardware pin mappings, APN credentials, broker addresses, and database credentials must be supplied by the user for an actual deployment.
