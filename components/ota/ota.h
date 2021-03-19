#ifndef __OTA_H__
#define __OTA_H__

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ota_start(char * ota_server_ip, int ota_server_port, char * ota_filename, void progress_cb(uint32_t));

#ifdef __cplusplus
}
#endif /*  extern "C" */

#endif /* __OTA_H__ */
