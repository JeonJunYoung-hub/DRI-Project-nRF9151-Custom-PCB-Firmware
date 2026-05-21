#include <stdio.h>
#include <string.h>
#include <ncs_version.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>
#include <nrf_modem_at.h>

//#include <dk_buttons_and_leds.h>
#include "mqtt_connection.h"
#include <config.h>
#include <zephyr/random/random.h>
#include <zephyr/IOT_PIPELINEvers/flash.h>



volatile bool mqtt_connected = false;

/* Buffers for MQTT client. */
static uint8_t rx_buffer[CONFIG_MQTT_MESSAGE_BUFFER_SIZE];
static uint8_t tx_buffer[CONFIG_MQTT_MESSAGE_BUFFER_SIZE];
static uint8_t payload_buf[CONFIG_MQTT_PAYLOAD_BUFFER_SIZE];

/* MQTT Broker details. */
static struct sockaddr_storage broker;

LOG_MODULE_DECLARE(IOT_PIPELINE);

/**@brief Function to get the payload of recived data.
 */
static int get_received_payload(struct mqtt_client *c, size_t length)
{
	int ret;
	int err = 0;

	/* Return an error if the payload is larger than the payload buffer.
	 * Note: To allow new messages, we have to read the payload before returning.
	 */
	if (length > sizeof(payload_buf)) {
		err = -EMSGSIZE;
	}

	/* Truncate payload until it fits in the payload buffer. */
	while (length > sizeof(payload_buf)) {
		ret = mqtt_read_publish_payload_blocking(
				c, payload_buf, (length - sizeof(payload_buf)));
		if (ret == 0) {
			return -EIO;
		} else if (ret < 0) {
			return ret;
		}

		length -= ret;
	}

	ret = mqtt_readall_publish_payload(c, payload_buf, length);
	if (ret) {
		return ret;
	}

	return err;
}

/**@brief Function to subscribe to the configured topic
 */
// static int subscribe(struct mqtt_client *const c)
// {
// 	struct mqtt_topic subscribe_topic = {
// 		.topic = {
// 			.utf8 = topic_sub,
// 			.size = strlen(topic_sub)
// 		},
// 		.qos = MQTT_QOS_1_AT_LEAST_ONCE
// 	};

// 	const struct mqtt_subscription_list subscription_list = {
// 		.list = &subscribe_topic,
// 		.list_count = 1,
// 		.message_id = 1234
// 	};

// 	LOG_INF("Subscribing to: %s len %u", topic_sub,
// 		(unsigned int)strlen(topic_sub));

// 	return mqtt_subscribe(c, &subscription_list);
// }

/**@brief Function to print strings without null-termination
 */
static void data_print(uint8_t *prefix, uint8_t *data, size_t len)
{
	char buf[len + 1];

	memcpy(buf, data, len);
	buf[len] = 0;
	printk("%s%s", (char *)prefix, (char *)buf);
}

/**@brief Function to publish data on the configured topic
 */
/* STEP 7.1 - Define the function data_publish() to publish data */
int data_publish(struct mqtt_client *c, enum mqtt_qos qos,
	uint8_t *data, size_t len, char *topic)
{
	struct mqtt_publish_param param;

	param.message.topic.qos = qos;
	param.message.topic.topic.utf8 = topic;
	param.message.topic.topic.size = strlen(topic);
	param.message.payload.data = data;
	param.message.payload.len = len;
	param.message_id = sys_rand32_get();
	param.dup_flag = 0;
	param.retain_flag = 0;

	data_print("[MongoDB] Publishing: \r\n", data, len);
	printk("To topic: %s len: %u\r\n",
		topic,
		(unsigned int)strlen(topic));
	printk("...\r\n");

	return mqtt_publish(c, &param);
}

int data_publish_satelite(struct mqtt_client *c, enum mqtt_qos qos,
	uint8_t *data, size_t len, char *topic)
{
	struct mqtt_publish_param param;

	param.message.topic.qos = qos;
	param.message.topic.topic.utf8 = topic;
	param.message.topic.topic.size = strlen(topic);
	param.message.payload.data = data;
	param.message.payload.len = len;
	param.message_id = sys_rand32_get();
	param.dup_flag = 0;
	param.retain_flag = 0;

	data_print("[GNSS] Publishing: \r\n", data, len);
	printk("[GNSS] To topic: %s len: %u\r\n",
		topic,
		(unsigned int)strlen(topic));
	printk("...\r\n");
	printk("...\r\n");
	
	return mqtt_publish(c, &param);
}

/**@brief MQTT client event handler
 */
void mqtt_evt_handler(struct mqtt_client *const c,
		      const struct mqtt_evt *evt)
{
	int err;

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
	/* STEP 5 - Subscribe to the topic CONFIG_MQTT_SUB_TOPIC when we have a successful connection */
		if (evt->result == 0) {
            mqtt_connected = true;
            LOG_INF("[MQTT] CONNACK received → connected");
        } else {
            mqtt_connected = false;
            LOG_ERR("[MQTT] CONNACK error: %d", evt->result);
        }
        break;
		

	case MQTT_EVT_DISCONNECT:
		mqtt_connected = false;
        LOG_WRN("[MQTT] Disconnected");
        break;

	case MQTT_EVT_PUBLISH:
	/* STEP 6 - Listen to published messages received from the broker and extract the message */
	{
		/* STEP 6.1 - Extract the payload */
		const struct mqtt_publish_param *p = &evt->param.publish;
		//Print the length of the recived message 
		LOG_INF("MQTT PUBLISH result=%d len=%d",
			evt->result, p->message.payload.len);

		//Extract the data of the recived message 
		err = get_received_payload(c, p->message.payload.len);
		
		//Send acknowledgment to the broker on receiving QoS1 publish message 
		if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
			const struct mqtt_puback_param ack = {
				.message_id = p->message_id
			};

			/* Send acknowledgment. */
			mqtt_publish_qos1_ack(c, &ack);
		}

		

		/* STEP 6.2 - On successful extraction of data */
		if (err >= 0) {
			data_print("Received: ", payload_buf, p->message.payload.len);
			
			
			

		// Payload buffer is smaller than the received data 
		} else if (err == -EMSGSIZE) {
			LOG_ERR("Received payload (%d bytes) is larger than the payload buffer size (%d bytes).",
				p->message.payload.len, sizeof(payload_buf));
		// Failed to extract data, disconnect 
		} else {
			LOG_ERR("get_received_payload failed: %d\r\n", err);
			LOG_INF("Disconnecting MQTT client...\r\n");

			err = mqtt_disconnect(c, NULL);
			if (err) {
				LOG_ERR("Could not disconnect: %d \r\n", err);
			}
		}
	} break;

	case MQTT_EVT_PUBACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT PUBACK error: %d", evt->result);
			break;
		}

		LOG_INF("PUBACK packet id: %u", evt->param.puback.message_id);
		break;

	case MQTT_EVT_SUBACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT SUBACK error: %d", evt->result);
			break;
		}

		LOG_INF("SUBACK packet id: %u", evt->param.suback.message_id);
		break;

	case MQTT_EVT_PINGRESP:
		if (evt->result != 0) {
			LOG_ERR("MQTT PINGRESP error: %d", evt->result);
		}
		break;

	default:
		LOG_INF("Unhandled MQTT event type: %d", evt->type);
		break;
	}
}


/**@brief Resolves the configured hostname and
 * initializes the MQTT broker structure
 */
 static int broker_init(void)
 {
 	int err;
 	struct adIOT_PIPELINEnfo *result;
 	struct adIOT_PIPELINEnfo *addr;
 	struct adIOT_PIPELINEnfo hints = {
 		.ai_family = AF_INET,  //ipv4 it is saying which one should i take
 		.ai_socktype = SOCK_STREAM //tcp
 	};

 	err = getadIOT_PIPELINEnfo(CONFIG_MQTT_BROKER_HOSTNAME, NULL, &hints, &result);
 	if (err) {
 		LOG_ERR("getadIOT_PIPELINEnfo failed: %d", err);
 		return -ECHILD;
 	}

 	addr = result;

 	/* Look for address of the broker. */
 	while (addr != NULL) {
 		/* IPv4 Address. */
 		if (addr->ai_addrlen == sizeof(struct sockaddr_in)) {
 			struct sockaddr_in *broker4 =
 				((struct sockaddr_in *)&broker);
 			char ipv4_addr[NET_IPV4_ADDR_LEN];

 			broker4->sin_addr.s_addr =
 				((struct sockaddr_in *)addr->ai_addr)
 				->sin_addr.s_addr;
 			broker4->sin_family = AF_INET; //ipv4
 			broker4->sin_port = htons(CONFIG_MQTT_BROKER_PORT); //port number

			inet_ntop(AF_INET, &broker4->sin_addr.s_addr, // Convert to read the person
 				  ipv4_addr, sizeof(ipv4_addr));
			
 			printk("[IPv4] TCP Address found %s\r\n", (char *)(ipv4_addr));
			k_sleep(K_MSEC(100));
 			break;
 		} else {
 			LOG_ERR("ai_addrlen = %u should be %u or %u",
 				(unsigned int)addr->ai_addrlen,
 				(unsigned int)sizeof(struct sockaddr_in),
 				(unsigned int)sizeof(struct sockaddr_in6));
 		}

		addr = addr->ai_next;
	}

 	/* Free the address. */
 	freeadIOT_PIPELINEnfo(result);

 	return err;
 }

/* Function to get the client id */
static const uint8_t* client_id_get(void)
{
	static uint8_t client_id[MAX(sizeof(CONFIG_MQTT_CLIENT_ID),
				     CLIENT_ID_LEN)];

	if (strlen(CONFIG_MQTT_CLIENT_ID) > 0) {
		snprintf(client_id, sizeof(client_id), "%s",
			 CONFIG_MQTT_CLIENT_ID);
		goto exit;
	}

	char imei_buf[CGSN_RESPONSE_LENGTH + 1];
	int err;

	err = nrf_modem_at_cmd(imei_buf, sizeof(imei_buf), "AT+CGSN");
	if (err) {
		LOG_ERR("Failed to obtain IMEI, error: %d", err);
		goto exit;
	}

	
	imei_buf[IMEI_LEN] = '\0';

	snprintf(client_id, sizeof(client_id), "nrf-%.*s", IMEI_LEN, imei_buf);

exit:
	LOG_DBG("client_id = %s", (char *)(client_id));

	return client_id;
}


/**@brief Initialize the MQTT client structure
 */

int client_init(struct mqtt_client *client)
{
	int err;
	/* Initializes the client instance. */
	mqtt_client_init(client);

	/* Resolves the configured hostname and initializes the MQTT broker structure */
	err = broker_init();
	if (err) {
		LOG_ERR("Failed to initialize broker connection");
		return err;
	}

	client->broker = &broker;
	client->evt_cb = mqtt_evt_handler;
	client->client_id.utf8 = client_id_get();
	client->client_id.size = strlen(client->client_id.utf8);
	client->password = NULL;
	client->user_name = NULL;
	client->protocol_version = MQTT_VERSION_3_1_1;
	 

	/* MQTT buffers configuration */
	client->rx_buf = rx_buffer;
	client->rx_buf_size = sizeof(rx_buffer);
	client->tx_buf = tx_buffer;
	client->tx_buf_size = sizeof(tx_buffer);

	/* We are not using TLS in Exercise 1 */
	client->transport.type = MQTT_TRANSPORT_NON_SECURE;


	return err;
}

/**@brief Initialize the file descriptor structure used by poll.
 */
int fds_init(struct mqtt_client *c, struct pollfd *fds)
{
	if (c->transport.type == MQTT_TRANSPORT_NON_SECURE) {
		fds->fd = c->transport.tcp.sock;
	} else {
		return -ENOTSUP;
	}

	fds->events = POLLIN;

	return 0;
}