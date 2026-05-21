import json
import os
import queue
import threading
from datetime import datetime
from zoneinfo import ZoneInfo

import paho.mqtt.client as mqtt
from pymongo.mongo_client import MongoClient
from pymongo.server_api import ServerApi


MONGODB_URI = os.environ["MONGODB_URI"]
MONGODB_DATABASE = os.getenv("MONGODB_DATABASE", "iot_sensor_pipeline")
MQTT_HOST = os.getenv("MQTT_HOST", "localhost")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_CLIENT_ID = os.getenv("MQTT_CLIENT_ID", "mqtt-mongodb-bridge")
MQTT_SENSOR_TOPIC = os.getenv("MQTT_SENSOR_TOPIC", "devices/+/telemetry")
MQTT_LOCATION_TOPIC = os.getenv("MQTT_LOCATION_TOPIC", "devices/+/location")
LOCAL_TIMEZONE = os.getenv("LOCAL_TIMEZONE", "UTC")


mongo_client = MongoClient(MONGODB_URI, server_api=ServerApi("1"))
db = mongo_client[MONGODB_DATABASE]

TOPIC_TO_COLLECTION = {
    MQTT_SENSOR_TOPIC: db["sensor_readings"],
    MQTT_LOCATION_TOPIC: db["device_locations"],
}

msg_queue = queue.Queue()


def normalize_device_id(raw_id: str) -> str:
    if not raw_id:
        return "unknown-device"
    return raw_id[:8]


def collection_for_topic(topic: str):
    for topic_filter, collection in TOPIC_TO_COLLECTION.items():
        if mqtt.topic_matches_sub(topic_filter, topic):
            return collection
    return db["unmatched_messages"]


def mongo_worker():
    while True:
        topic, document = msg_queue.get()
        try:
            collection_for_topic(topic).insert_one(document)
            print(f"Saved MQTT message from {topic}")
        except Exception as exc:
            print(f"MongoDB insert failed: {exc}")
        finally:
            msg_queue.task_done()


def on_connect(client, userdata, flags, reason_code):
    print(f"MQTT connected with result code {reason_code}")
    client.subscribe(MQTT_SENSOR_TOPIC)
    client.subscribe(MQTT_LOCATION_TOPIC)


def on_message(client, userdata, msg):
    try:
        payload = msg.payload.decode("utf-8")
        data = json.loads(payload)
        device_id = normalize_device_id(data.pop("_id", data.get("ID", "")))

        document = {
            "device_id": device_id,
            "topic": msg.topic,
            "received_at": datetime.now(ZoneInfo(LOCAL_TIMEZONE)).isoformat(),
            "payload": data,
        }

        msg_queue.put((msg.topic, document))
    except Exception as exc:
        print(f"MQTT message handling failed: {exc}")


def main():
    mongo_client.admin.command("ping")
    threading.Thread(target=mongo_worker, daemon=True).start()

    mqtt_client = mqtt.Client(
        client_id=MQTT_CLIENT_ID,
        clean_session=False,
        protocol=mqtt.MQTTv311,
    )
    mqtt_client.on_connect = on_connect
    mqtt_client.on_message = on_message
    mqtt_client.connect(MQTT_HOST, MQTT_PORT, 60)
    mqtt_client.loop_forever()


if __name__ == "__main__":
    main()
