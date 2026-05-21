# MQTT to MongoDB Bridge

Small Python service that subscribes to MQTT telemetry topics and writes normalized documents into MongoDB.

## Configuration

Copy `.env.example` into your local environment and provide real values outside of Git.

Required:

- `MONGODB_URI`
- `MQTT_HOST`

Optional values such as topic filters, database name, port, and timezone have safe defaults for local testing.

## Run

```powershell
pip install -r requirements.txt
$env:MONGODB_URI="<your MongoDB connection string>"
$env:MQTT_HOST="localhost"
python bridge.py
```
