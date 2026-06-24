#include "esp_err.h"

esp_err_t webserver_init(void);
esp_err_t webserver_restart_wifi(void);  // Reconfigure and restart AP with current params
esp_err_t webserver_stop(void);          // Stop HTTP server AND WiFi (hibernate/shutdown only)
esp_err_t webserver_sleep(void);         // Stop HTTP server, keep WiFi AP up (soft idle)
esp_err_t webserver_wake(void);          // Restart HTTP server after webserver_sleep()