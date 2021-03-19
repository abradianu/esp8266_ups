#ifndef __MQTT_CLIENT_H__
#define __MQTT_CLIENT_H__

#include "lwip/apps/mqtt.h"

#ifdef	__cplusplus
extern "C" {
#endif

typedef struct {
    char *  broker;
    char *  user;
    char *  pwd;
    char *  sub_topic;
    char *  client_id;
    uint8_t sub_qos;
    void    (*message_received_cb)(const uint8_t *data, u16_t len, bool last);
    void *  ctrl_handle;
} mqtt_client_info_t;

/* mqtt_client_ctrl values need to be static allocated */
esp_err_t mqtt_client_start(mqtt_client_info_t * mqtt_client_ctrl);

esp_err_t mqtt_client_publish(void * ctrl_handle, const char *topic,
        const uint8_t *data, u16_t len, uint8_t qos, uint8_t retain);

#ifdef	__cplusplus
extern "C" {
#endif

#endif /* __MQTT_CLIENT_H__ */
