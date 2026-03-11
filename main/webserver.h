#include "esp_err.h"

esp_err_t webserver_init(void);
esp_err_t webserver_restart_wifi(void);  // Reconfigure and restart AP with current params
esp_err_t webserver_stop(void);          // Stop HTTP server and WiFi (for soft idle)