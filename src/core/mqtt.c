/**
 * @file mqtt.c
 * @brief MQTT core implementation
 */

#include "mqtt.h"
#include <string.h>

#define MQTT_CONNECT     1
#define MQTT_CONNACK     2
#define MQTT_PUBLISH     3
#define MQTT_PUBACK      4
#define MQTT_PUBREC      5
#define MQTT_PUBREL      6
#define MQTT_PUBCOMP     7
#define MQTT_SUBSCRIBE   8
#define MQTT_SUBACK      9
#define MQTT_PINGREQ     12
#define MQTT_PINGRESP    13
#define MQTT_DISCONNECT  14

#define MQTT_CONNECT_TIMEOUT_MS     5000
#define MQTT_RECONNECT_DELAY_MS     1000
#define MQTT_RECV_TIMEOUT_MS        1000

static void mqtt_recv_thread(void* arg);

/* Helper function to handle uint32_t timestamp wraparound */
static inline int mqtt_time_elapsed(uint32_t start, uint32_t now, uint32_t threshold) {
    uint32_t elapsed = now - start;  /* Handles wraparound correctly */
    return elapsed >= threshold;
}

static int encode_remaining_length(uint8_t* buf, size_t len) {
    int count = 0;
    do {
        uint8_t byte = len % 128;
        len /= 128;
        if (len > 0) byte |= 0x80;
        buf[count++] = byte;
    } while (len > 0);
    return count;
}

static int decode_remaining_length(const uint8_t* buf, size_t* len) {
    int multiplier = 1, count = 0;
    *len = 0;
    uint8_t byte;
    do {
        if (count >= 4) return -1;
        byte = buf[count++];
        *len += (byte & 127) * multiplier;
        multiplier *= 128;
    } while ((byte & 128) != 0);
    return count;
}

static int pack_connect(uint8_t* buf, const char* client_id, const char* username,
                        const char* password, uint16_t keepalive, uint8_t clean_session) {
    int pos = 0;
    size_t cid_len = strlen(client_id);
    size_t payload_len = 2 + cid_len;
    uint8_t flags = clean_session ? 0x02 : 0x00;
    
    if (username) {
        payload_len += 2 + strlen(username);
        flags |= 0x80;
    }
    if (password) {
        payload_len += 2 + strlen(password);
        flags |= 0x40;
    }
    
    size_t remaining = 10 + payload_len;
    uint8_t rem_buf[4];
    int rem_len = encode_remaining_length(rem_buf, remaining);
    
    buf[pos++] = MQTT_CONNECT << 4;
    memcpy(buf + pos, rem_buf, rem_len);
    pos += rem_len;
    
    buf[pos++] = 0; buf[pos++] = 4;
    memcpy(buf + pos, "MQTT", 4); pos += 4;
    buf[pos++] = 4;
    buf[pos++] = flags;
    buf[pos++] = keepalive >> 8;
    buf[pos++] = keepalive & 0xFF;
    
    buf[pos++] = cid_len >> 8;
    buf[pos++] = cid_len & 0xFF;
    memcpy(buf + pos, client_id, cid_len);
    pos += cid_len;
    
    if (username) {
        size_t len = strlen(username);
        buf[pos++] = len >> 8;
        buf[pos++] = len & 0xFF;
        memcpy(buf + pos, username, len);
        pos += len;
    }
    if (password) {
        size_t len = strlen(password);
        buf[pos++] = len >> 8;
        buf[pos++] = len & 0xFF;
        memcpy(buf + pos, password, len);
        pos += len;
    }
    
    return pos;
}

static int pack_publish(uint8_t* buf, const char* topic, const uint8_t* payload,
                        size_t payload_len, uint8_t qos, uint16_t packet_id) {
    int pos = 0;
    size_t topic_len = strlen(topic);
    size_t remaining = 2 + topic_len + payload_len;
    if (qos > 0) remaining += 2;
    
    uint8_t rem_buf[4];
    int rem_len = encode_remaining_length(rem_buf, remaining);
    
    buf[pos++] = (MQTT_PUBLISH << 4) | (qos << 1);
    memcpy(buf + pos, rem_buf, rem_len);
    pos += rem_len;
    
    buf[pos++] = topic_len >> 8;
    buf[pos++] = topic_len & 0xFF;
    memcpy(buf + pos, topic, topic_len);
    pos += topic_len;
    
    if (qos > 0) {
        buf[pos++] = packet_id >> 8;
        buf[pos++] = packet_id & 0xFF;
    }
    
    memcpy(buf + pos, payload, payload_len);
    pos += payload_len;
    
    return pos;
}

static int pack_subscribe(uint8_t* buf, const char* topic, uint8_t qos, uint16_t packet_id) {
    int pos = 0;
    size_t topic_len = strlen(topic);
    size_t remaining = 2 + 2 + topic_len + 1;
    
    uint8_t rem_buf[4];
    int rem_len = encode_remaining_length(rem_buf, remaining);
    
    buf[pos++] = (MQTT_SUBSCRIBE << 4) | 0x02;
    memcpy(buf + pos, rem_buf, rem_len);
    pos += rem_len;
    
    buf[pos++] = packet_id >> 8;
    buf[pos++] = packet_id & 0xFF;
    buf[pos++] = topic_len >> 8;
    buf[pos++] = topic_len & 0xFF;
    memcpy(buf + pos, topic, topic_len);
    pos += topic_len;
    buf[pos++] = qos;
    
    return pos;
}

static int pack_pingreq(uint8_t* buf) {
    buf[0] = MQTT_PINGREQ << 4;
    buf[1] = 0;
    return 2;
}

static int pack_disconnect(uint8_t* buf) {
    buf[0] = MQTT_DISCONNECT << 4;
    buf[1] = 0;
    return 2;
}

static int pack_puback(uint8_t* buf, uint16_t packet_id) {
    buf[0] = MQTT_PUBACK << 4;
    buf[1] = 2;
    buf[2] = packet_id >> 8;
    buf[3] = packet_id & 0xFF;
    return 4;
}

static int pack_pubrec(uint8_t* buf, uint16_t packet_id) {
    buf[0] = MQTT_PUBREC << 4;
    buf[1] = 2;
    buf[2] = packet_id >> 8;
    buf[3] = packet_id & 0xFF;
    return 4;
}

static int pack_pubrel(uint8_t* buf, uint16_t packet_id) {
    buf[0] = (MQTT_PUBREL << 4) | 0x02;
    buf[1] = 2;
    buf[2] = packet_id >> 8;
    buf[3] = packet_id & 0xFF;
    return 4;
}

static int pack_pubcomp(uint8_t* buf, uint16_t packet_id) {
    buf[0] = MQTT_PUBCOMP << 4;
    buf[1] = 2;
    buf[2] = packet_id >> 8;
    buf[3] = packet_id & 0xFF;
    return 4;
}

mqtt_client_t* mqtt_client_create(const mqtt_config_t* config) {
    const mqtt_os_api_t* os = mqtt_os_get();
    const mqtt_net_api_t* net = mqtt_net_get();
    if (!os || !net) return NULL;
    
    mqtt_client_t* client = (mqtt_client_t*)os->malloc(sizeof(mqtt_client_t));
    if (!client) return NULL;
    
    memset(client, 0, sizeof(mqtt_client_t));
    memcpy(&client->config, config, sizeof(mqtt_config_t));
    client->state = MQTT_STATE_DISCONNECTED;
    client->packet_id = 1;
    client->sub_count = 0;
    
    client->mutex = os->mutex_create();
    if (!client->mutex) goto err_free_client;
    
    client->thread_exit_sem = os->sem_create(0);
    if (!client->thread_exit_sem) goto err_destroy_mutex;
    
    client->socket = net->connect(client->config.host, client->config.port, MQTT_CONNECT_TIMEOUT_MS);
    if (!client->socket) goto err_destroy_sem;
    
    int len = pack_connect(client->send_buf, client->config.client_id, client->config.username,
                           client->config.password, client->config.keepalive, client->config.clean_session);
    
    if (net->send(client->socket, client->send_buf, len) != len) goto err_disconnect;
    
    int recv_len = net->recv(client->socket, client->recv_buf, MQTT_RECV_BUF_SIZE, MQTT_CONNECT_TIMEOUT_MS);
    if (recv_len < 4 || (client->recv_buf[0] >> 4) != MQTT_CONNACK || client->recv_buf[3] != 0) {
        goto err_disconnect;
    }
    
    client->state = MQTT_STATE_CONNECTED;
    client->last_ping_time = os->get_time_ms();
    client->running = 1;
    
    client->recv_thread = os->thread_create(mqtt_recv_thread, client, 2048, 5);
    if (!client->recv_thread) goto err_disconnect;
    
    return client;

err_disconnect:
    net->disconnect(client->socket);
err_destroy_sem:
    os->sem_destroy(client->thread_exit_sem);
err_destroy_mutex:
    os->mutex_destroy(client->mutex);
err_free_client:
    os->free(client);
    return NULL;
}

void mqtt_client_destroy(mqtt_client_t* client) {
    if (!client) return;
    
    const mqtt_os_api_t* os = mqtt_os_get();
    const mqtt_net_api_t* net = mqtt_net_get();
    
    client->running = 0;
    
    if (client->recv_thread) {
        os->sem_wait(client->thread_exit_sem);
        os->thread_destroy(client->recv_thread);
    }
    
    if (client->socket) {
        int len = pack_disconnect(client->send_buf);
        net->send(client->socket, client->send_buf, len);
        net->disconnect(client->socket);
    }
    
    if (client->thread_exit_sem) os->sem_destroy(client->thread_exit_sem);
    if (client->mutex) os->mutex_destroy(client->mutex);
    os->free(client);
}

int mqtt_client_subscribe(mqtt_client_t* client, const char* topic, uint8_t qos) {
    if (!client || client->state != MQTT_STATE_CONNECTED) return -1;
    
    const mqtt_os_api_t* os = mqtt_os_get();
    const mqtt_net_api_t* net = mqtt_net_get();
    
    os->mutex_lock(client->mutex);
    
    int len = pack_subscribe(client->send_buf, topic, qos, client->packet_id++);
    int ret = net->send(client->socket, client->send_buf, len);
    
    if (ret == len && client->sub_count < MQTT_MAX_SUBSCRIPTIONS) {
        int found = 0;
        for (int i = 0; i < client->sub_count; i++) {
            if (strcmp(client->subscriptions[i].topic, topic) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            strncpy(client->subscriptions[client->sub_count].topic, topic, sizeof(client->subscriptions[0].topic) - 1);
            client->subscriptions[client->sub_count].qos = qos;
            client->sub_count++;
        }
    }
    
    os->mutex_unlock(client->mutex);
    
    return (ret == len) ? 0 : -1;
}

int mqtt_client_publish(mqtt_client_t* client, const char* topic, const uint8_t* payload,
                        size_t len, uint8_t qos) {
    if (!client || client->state != MQTT_STATE_CONNECTED) return -1;
    
    const mqtt_os_api_t* os = mqtt_os_get();
    const mqtt_net_api_t* net = mqtt_net_get();
    
    os->mutex_lock(client->mutex);
    
    uint16_t packet_id = (qos > 0) ? client->packet_id++ : 0;
    int pkt_len = pack_publish(client->send_buf, topic, payload, len, qos, packet_id);
    int ret = net->send(client->socket, client->send_buf, pkt_len);
    
    os->mutex_unlock(client->mutex);
    
    return (ret == pkt_len) ? 0 : -1;
}

int mqtt_client_is_connected(mqtt_client_t* client) {
    return client && client->state == MQTT_STATE_CONNECTED;
}

static int mqtt_try_reconnect(mqtt_client_t* client) {
    const mqtt_net_api_t* net = mqtt_net_get();
    const mqtt_os_api_t* os = mqtt_os_get();
    int len;
    
    client->socket = net->connect(client->config.host, client->config.port, MQTT_CONNECT_TIMEOUT_MS);
    if (!client->socket) return -1;
    
    os->mutex_lock(client->mutex);
    
    len = pack_connect(client->send_buf, client->config.client_id, client->config.username,
                       client->config.password, client->config.keepalive, client->config.clean_session);
    
    if (net->send(client->socket, client->send_buf, len) != len) goto err_cleanup;
    
    int recv_len = net->recv(client->socket, client->recv_buf, MQTT_RECV_BUF_SIZE, MQTT_CONNECT_TIMEOUT_MS);
    if (recv_len < 4 || (client->recv_buf[0] >> 4) != MQTT_CONNACK || client->recv_buf[3] != 0) {
        goto err_cleanup;
    }
    
    for (int i = 0; i < client->sub_count; i++) {
        len = pack_subscribe(client->send_buf, client->subscriptions[i].topic,
                             client->subscriptions[i].qos, client->packet_id++);
        if (net->send(client->socket, client->send_buf, len) != len) goto err_cleanup;
    }
    
    client->state = MQTT_STATE_CONNECTED;
    client->last_ping_time = os->get_time_ms();
    client->waiting_pingresp = 0;
    
    os->mutex_unlock(client->mutex);
    return 0;

err_cleanup:
    net->disconnect(client->socket);
    client->socket = NULL;
    os->mutex_unlock(client->mutex);
    return -1;
}

static int mqtt_send_ping(mqtt_client_t* client) {
    const mqtt_net_api_t* net = mqtt_net_get();
    const mqtt_os_api_t* os = mqtt_os_get();
    
    uint32_t now = os->get_time_ms();
    uint32_t keepalive_ms = client->config.keepalive * 1000;
    
    if (client->waiting_pingresp) {
        if (mqtt_time_elapsed(client->ping_sent_time, now, keepalive_ms / 2)) {
            os->mutex_lock(client->mutex);
            client->state = MQTT_STATE_DISCONNECTED;
            client->waiting_pingresp = 0;
            net->disconnect(client->socket);
            client->socket = NULL;
            os->mutex_unlock(client->mutex);
            return -1;
        }
        return 0;
    }
    
    if (!mqtt_time_elapsed(client->last_ping_time, now, keepalive_ms / 2)) {
        return 0;
    }
    
    os->mutex_lock(client->mutex);
    
    int len = pack_pingreq(client->send_buf);
    if (net->send(client->socket, client->send_buf, len) != len) {
        client->state = MQTT_STATE_DISCONNECTED;
        net->disconnect(client->socket);
        client->socket = NULL;
        os->mutex_unlock(client->mutex);
        return -1;
    }
    client->ping_sent_time = now;
    client->waiting_pingresp = 1;
    
    os->mutex_unlock(client->mutex);
    return 0;
}

static void mqtt_handle_publish(mqtt_client_t* client, int len) {
    const mqtt_net_api_t* net = mqtt_net_get();
    const mqtt_os_api_t* os = mqtt_os_get();
    
    size_t remaining;
    int offset = 1 + decode_remaining_length(client->recv_buf + 1, &remaining);
    
    uint16_t topic_len = (client->recv_buf[offset] << 8) | client->recv_buf[offset + 1];
    offset += 2;
    
    char topic[128];
    if (topic_len >= sizeof(topic)) return;
    
    memcpy(topic, client->recv_buf + offset, topic_len);
    topic[topic_len] = '\0';
    offset += topic_len;
    
    uint8_t qos = (client->recv_buf[0] >> 1) & 0x03;
    uint16_t packet_id = 0;
    if (qos > 0) {
        packet_id = (client->recv_buf[offset] << 8) | client->recv_buf[offset + 1];
        offset += 2;
    }
    
    size_t payload_len = len - offset;
    if (client->config.msg_cb) {
        client->config.msg_cb(topic, client->recv_buf + offset, payload_len, client->config.user_data);
    }
    
    os->mutex_lock(client->mutex);
    if (qos == 1) {
        int pkt_len = pack_puback(client->send_buf, packet_id);
        net->send(client->socket, client->send_buf, pkt_len);
    } else if (qos == 2) {
        int pkt_len = pack_pubrec(client->send_buf, packet_id);
        net->send(client->socket, client->send_buf, pkt_len);
    }
    os->mutex_unlock(client->mutex);
}

static void mqtt_recv_thread(void* arg) {
    mqtt_client_t* client = (mqtt_client_t*)arg;
    const mqtt_net_api_t* net = mqtt_net_get();
    const mqtt_os_api_t* os = mqtt_os_get();
    
    while (client->running) {
        if (client->state == MQTT_STATE_DISCONNECTED) {
            if (mqtt_try_reconnect(client) != 0) {
                os->sleep_ms(MQTT_RECONNECT_DELAY_MS);
            }
            continue;
        }
        
        if (mqtt_send_ping(client) != 0) {
            continue;
        }
        
        int len = net->recv(client->socket, client->recv_buf, MQTT_RECV_BUF_SIZE, MQTT_RECV_TIMEOUT_MS);
        
        if (len < 0) {
            os->mutex_lock(client->mutex);
            if (client->state == MQTT_STATE_CONNECTED) {
                client->state = MQTT_STATE_DISCONNECTED;
                net->disconnect(client->socket);
                client->socket = NULL;
            }
            os->mutex_unlock(client->mutex);
            continue;
        }
        if (len == 0) continue;
        
        uint8_t type = client->recv_buf[0] >> 4;
        
        if (type == MQTT_PINGRESP) {
            client->last_ping_time = os->get_time_ms();
            client->waiting_pingresp = 0;
            continue;
        }
        
        if (type == MQTT_PUBLISH) {
            mqtt_handle_publish(client, len);
        } else if (type == MQTT_PUBREL) {
            if (len >= 4) {
                uint16_t packet_id = (client->recv_buf[2] << 8) | client->recv_buf[3];
                os->mutex_lock(client->mutex);
                int pkt_len = pack_pubcomp(client->send_buf, packet_id);
                net->send(client->socket, client->send_buf, pkt_len);
                os->mutex_unlock(client->mutex);
            }
        }
    }
    
    /* Signal thread exit completion */
    os->sem_post(client->thread_exit_sem);
    
    /* Call thread_exit - RTOS implementation handles cleanup properly */
    if (os->thread_exit) {
        os->thread_exit();
    }
}
