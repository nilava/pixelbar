// Firmware updates over the network. Private to the net component.
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ota_post(httpd_req_t* r);

// -1 when idle, otherwise 0..1 through an upload.
float ota_progress(void);

// Confirms the running image so the bootloader stops holding the previous one
// in reserve. Called only once the render loop has actually survived a while;
// calling it at startup would defeat the entire point of rollback.
void ota_mark_healthy(void);

#ifdef __cplusplus
}
#endif
