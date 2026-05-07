#ifndef SETUP_SERVICE_H
#define SETUP_SERVICE_H

#include <esp_http_server.h>

/**
 * SetupService - registers custom HTTP routes for schedule/weather config
 * Call SetupService::RegisterRoutes(httpd_handle_t) during WiFi config mode
 */
namespace SetupService {

/**
 * Register custom setup routes on the given HTTP server
 * Routes: /schedule.html, /weather.html, /done.html
 * API:     /api/config/status, /api/schedule, /api/weather
 */
void RegisterRoutes(httpd_handle_t server);

} // namespace SetupService

#endif
