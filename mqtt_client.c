#include "libemqtt.h"
#include "mqtt_client.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/tcp.h>
#include <signal.h>
#include <pthread.h>
#include <stdlib.h>

#define LOGS(TAG, fmt, ...)          fprintf(stdout, "" fmt "\n", ##__VA_ARGS__)
#define LOGT(TAG, fmt, ...)          fprintf(stdout, "\033[0m\033[42;33mI\033[0m/ %s" fmt " at %s:%u\n", TAG, ##__VA_ARGS__, __PRETTY_FUNCTION__, __LINE__)
#define LOGI(TAG, fmt, ...)          fprintf(stdout, "\033[0m\033[42;33mI\033[0m/ %s" fmt " at %s:%u\n", TAG, ##__VA_ARGS__, __PRETTY_FUNCTION__, __LINE__)
#define LOGD(TAG, fmt, ...)          fprintf(stdout, "\033[0m\033[47;33mD\033[0m/ %s" fmt " at %s:%u\n", TAG, ##__VA_ARGS__, __PRETTY_FUNCTION__, __LINE__)
#define LOGE(TAG, fmt, ...)          fprintf(stdout, "\033[0m\033[41;33mE\033[0m/ %s" fmt " at %s:%u\n", TAG, ##__VA_ARGS__, __PRETTY_FUNCTION__, __LINE__)
#define LOGW(TAG, fmt, ...)          fprintf(stdout, "\033[0m\033[41;33mW\033[0m/ %s" fmt " at %s:%u\n", TAG, ##__VA_ARGS__, __PRETTY_FUNCTION__, __LINE__)

#define TAG "[mqtt-cli]"

#define RCVBUFSIZE 1024

static mqtt_broker_handle_t sub_broker = {0};

static int send_packet(void* socket_info, const void* buf, unsigned int count)
{
	LOGD(TAG, "---send[%d]---", count);
	int fd = *((int*)socket_info);
	return send(fd, buf, count, 0);
}

static int init_socket(mqtt_broker_handle_t* broker, const char* hostname, short port, int *socket_id, int keepalive)
{
	int flag = 1;

	// Create the socket
	if ((*socket_id = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        LOGE(TAG, "socket init failed");
        return -1;
    }

	// Disable Nagle Algorithm
	if (setsockopt(*socket_id, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag)) < 0) {
        LOGE(TAG, "socket setsockopt failed");
		return -1;
    }

	struct sockaddr_in socket_address;
	// Create the stuff we need to connect
	socket_address.sin_family = AF_INET;
	socket_address.sin_port = htons(port);
	socket_address.sin_addr.s_addr = inet_addr(hostname);

	// Connect the socket
	int err;
	if ((err = connect(*socket_id, (struct sockaddr*)&socket_address, sizeof(socket_address))) < 0) {
        LOGE(TAG, "socket connect failed. err=%d", err);
		return -1;
	}

	// MQTT stuffs
	mqtt_set_alive(broker, keepalive);
	broker->socket_info = (void*)socket_id;
	broker->send = send_packet;

	return 0;
}

int read_packet(int timeout, int socket_id, uint8_t *packet_buffer)
{
    int total_bytes = 0, bytes_rcvd, packet_length;
	if(timeout > 0)
	{
		fd_set readfds;
		struct timeval tmv;
        int ret;

		// Initialize the timeout data structure
		tmv.tv_sec = timeout;
		tmv.tv_usec = 0;

    L_SELECT:
        // Initialize the file descriptor set
        FD_ZERO (&readfds);
		FD_SET (socket_id, &readfds);
		// select returns 0 if timeout, 1 if input available, -1 if error
		ret = select(socket_id + 1, &readfds, NULL, NULL, &tmv);
		if (ret < 0) return -1;
        else if (ret == 0) return 0;
        else {
            if (FD_ISSET(socket_id, &readfds)) {
                LOGD(TAG, "FD SET[fd=%d], tmo=%d,%d", socket_id, (int)tmv.tv_sec, (int)tmv.tv_usec);
                goto L_READ;
            } else {
                LOGW(TAG, "FD NOT SET, tmo=%d,%d", (int)tmv.tv_sec, (int)tmv.tv_usec);
                goto L_SELECT;
            }
        }
	}

L_READ:
	memset(packet_buffer, 0, RCVBUFSIZE);

	while (total_bytes < 2) {// Reading fixed header
		if ((bytes_rcvd = recv(socket_id, (packet_buffer+total_bytes), RCVBUFSIZE, 0)) <= 0) {
            LOGE(TAG, "socket recv error");
			return -1;
        }

        LOGD(TAG, "recv=%d", bytes_rcvd);
		total_bytes += bytes_rcvd; // Keep tally of total bytes
	}

	packet_length = packet_buffer[1] + 2; // Remaining length + fixed header length

	while(total_bytes < packet_length) {// Reading the packet
		if ((bytes_rcvd = recv(socket_id, (packet_buffer+total_bytes), RCVBUFSIZE, 0)) <= 0) {
            LOGE(TAG, "socket recv err");
			return -1;
        }

        LOGD(TAG, "recv=%d", bytes_rcvd);
		total_bytes += bytes_rcvd; // Keep tally of total bytes
	}

	return packet_length;
}

static int close_socket(mqtt_broker_handle_t* broker)
{
	int fd = *((int*)broker->socket_info);
	return close(fd);
}

int mqtt_cli_subscribe()
{
    return 0;
}

int mqtt_cli_publish()
{
    int packet_length;
	uint16_t msg_id, msg_id_rcv;
	mqtt_broker_handle_t broker = {0};
    int socket_id = -1;
    uint8_t packet_buffer[RCVBUFSIZE];

    mqtt_init(&broker, "avc_cli_pub_001");
	init_socket(&broker, "10.37.129.2", 1883, &socket_id, 3);

    LOGD(TAG, "sock=%d", socket_id);

    mqtt_connect(&broker);

    packet_length = read_packet(1, socket_id, packet_buffer);
	if (packet_length < 0) {
		LOGE(TAG, "Error(%d) on read packet!", packet_length);
		return -1;
	}

    if (MQTTParseMessageType(packet_buffer) != MQTT_MSG_CONNACK) {
		LOGE(TAG, "CONNACK expected!");
		return -1;
	}

	if (packet_buffer[3] != 0x00) {
		LOGE(TAG, "CONNACK failed!");
		return -1;
	}

    LOGT(TAG, "Publish: QoS 2");
    char msg[64];
    static int cnt = 0;
    snprintf(msg, sizeof(msg), "msg Qos2. cnt_%d", ++cnt);
	mqtt_publish_with_qos(&broker, "mqtt_test_topic", msg, 1, 2, &msg_id); // Retain
	packet_length = read_packet(1, socket_id, packet_buffer);
	if (packet_length < 0) {
		LOGE(TAG, "Error(%d) on read packet", packet_length);
		return -1;
	}

	if (MQTTParseMessageType(packet_buffer) != MQTT_MSG_PUBREC) {
		LOGE(TAG, "PUBREC expected!");
		return -1;
	}

	msg_id_rcv = mqtt_parse_msg_id(packet_buffer);
	if (msg_id != msg_id_rcv) {
		LOGE(TAG, "%d message id was expected, but %d message id was found!", msg_id, msg_id_rcv);
		return -1;
	}

	mqtt_pubrel(&broker, msg_id);
	packet_length = read_packet(1, socket_id, packet_buffer);
	if (packet_length < 0) {
		LOGE(TAG, "Error(%d) on read packet!", packet_length);
		return -1;
	}

	if (MQTTParseMessageType(packet_buffer) != MQTT_MSG_PUBCOMP) {
		LOGE(TAG, "PUBCOMP expected!");
		return -1;
	}

	msg_id_rcv = mqtt_parse_msg_id(packet_buffer);
	if (msg_id != msg_id_rcv) {
		LOGE(TAG, "%d message id was expected, but %d message id was found!", msg_id, msg_id_rcv);
		return -1;
	}

	mqtt_disconnect(&broker);
	close_socket(&broker);
    return 0;
}

void alive(int sig)
{
	LOGT(TAG, "Timeout! Sending ping...");
	mqtt_ping(&sub_broker);

	alarm(30);
}

void term(int sig)
{
	LOGT(TAG, "Goodbye!");
	mqtt_disconnect(&sub_broker);
	close_socket(&sub_broker);

	exit(0);
}

static void* __subscribe_task(void *arg)
{
    int socket_id = (int)arg;
    int packet_length;
    uint8_t packet_buffer[RCVBUFSIZE];
    int recv_cnt = 0;

    LOGW(TAG, "fd=%d", socket_id);
    while (1) {
        packet_length = read_packet(0, socket_id, packet_buffer);
		if (packet_length == -1) {
			LOGE(TAG, "Error(%d) on read packet!", packet_length);
		} else if(packet_length > 0) {
			LOGD(TAG, "Packet Header: 0x%x...", packet_buffer[0]);
			if (MQTTParseMessageType(packet_buffer) == MQTT_MSG_PUBLISH) {
				uint8_t topic[255], msg[1000];
				uint16_t len;
				len = mqtt_parse_pub_topic(packet_buffer, topic);
				topic[len] = '\0'; // for printf
				len = mqtt_parse_publish_msg(packet_buffer, msg);
				msg[len] = '\0'; // for printf
				LOGT(TAG, "[%d] %s %s", ++recv_cnt, topic, msg);
			}
		}
    }

    return NULL;
}

static int __subscribe_init()
{
    int packet_length;
	uint16_t msg_id, msg_id_rcv;
    uint8_t packet_buffer[RCVBUFSIZE];
    pthread_t pid;
    int socket_id = -1;

	mqtt_init(&sub_broker, "avc_cli_sub_001");
    init_socket(&sub_broker, "10.37.129.2", 1883, &socket_id, 30);
    mqtt_connect(&sub_broker);

    LOGW(TAG, "sub sock=%d", socket_id);

    packet_length = read_packet(1, socket_id, packet_buffer);
	if (packet_length < 0) {
		LOGE(TAG, "Error(%d) on read packet!", packet_length);
		return -1;
	}

    if (MQTTParseMessageType(packet_buffer) != MQTT_MSG_CONNACK) {
		LOGE(TAG, "CONNACK expected!");
		return -1;
	}

	if (packet_buffer[3] != 0x00) {
		LOGE(TAG, "CONNACK failed!");
		return -1;
	}

    signal(SIGALRM, alive);
	alarm(30);
	signal(SIGINT, term);

    mqtt_subscribe(&sub_broker, "mqtt_test_topic", &msg_id);
    packet_length = read_packet(1, socket_id, packet_buffer);
    if (packet_length < 0) {
		LOGE(TAG, "Error(%d) on read packet!", packet_length);
		return -1;
	}

    if (MQTTParseMessageType(packet_buffer) != MQTT_MSG_SUBACK) {
		LOGE(TAG, "SUBACK expected!");
		return -1;
	}

    msg_id_rcv = mqtt_parse_msg_id(packet_buffer);
	if (msg_id != msg_id_rcv) {
		LOGE(TAG, "%d message id was expected, but %d message id was found!", msg_id, msg_id_rcv);
		return -1;
	}

    pthread_create(&pid, NULL, __subscribe_task, (void *)socket_id);
    pthread_detach(pid);

    usleep(1000 * 1000 * 2);
    return 0;
}

int main()
{
    int count = 0;

    __subscribe_init();

    while (1) {
        LOGD(TAG, "===================count=%d", ++count);
        mqtt_cli_publish();
        usleep(1000 * 50);
    }

    return 0;
}