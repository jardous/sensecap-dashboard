#include "network/internal.hpp"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
namespace dash::platform {
static std::array<HostConfig, 4> hosts;
static bool tcp(const HostConfig& h) {
    addrinfo hints{}; hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_INET;
    addrinfo* result = nullptr;
    if (getaddrinfo(h.address, nullptr, &hints, &result)) return false;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(result); return false; }
    auto address = *reinterpret_cast<sockaddr_in*>(result->ai_addr);
    freeaddrinfo(result); address.sin_port = htons(h.port);
    bool ok = false;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) == 0) {
        const int rc = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        if (rc == 0) ok = true;
        else if (errno == EINPROGRESS) {
            fd_set writable; FD_ZERO(&writable); FD_SET(fd, &writable);
            timeval timeout{1, 500000};
            if (select(fd + 1, nullptr, &writable, nullptr, &timeout) > 0) {
                int error = 0; socklen_t size = sizeof(error);
                ok = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && !error;
            }
        }
    }
    close(fd); return ok;
}
static bool http(const HostConfig& h) {
    esp_http_client_config_t config{};
    config.url = h.address; config.timeout_ms = 1500;
    config.method = HTTP_METHOD_HEAD; config.disable_auto_redirect = true;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    auto client = esp_http_client_init(&config);
    if (!client) return false;
    const auto error = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return error == ESP_OK && status >= 200 && status < 300;
}
static void worker(void*) {
    for (;;) {
        for (size_t i = 0; i < hosts.size(); ++i) {
            if (!hosts[i].address[0] || !wifi_connected()) continue;
            const auto begin = now_ms();
            const bool up = hosts[i].check == Check::Tcp ? tcp(hosts[i]) : http(hosts[i]);
            update_host(i, up, float(now_ms() - begin), now_ms());
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
void start_hosts(const Settings& s) {
    hosts = s.hosts;
    configASSERT(xTaskCreate(worker, "host_checks", 6144, nullptr, 2, nullptr) == pdPASS);
}
}
