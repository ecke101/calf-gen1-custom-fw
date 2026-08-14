#include "ngcd.h"
#include "ngcd_rk.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void handle_signal(int signal_number)
{
    (void)signal_number;
    ngcd_request_shutdown();
}

static void handle_fatal_signal(int signal_number)
{
    (void)ngcd_select_stock_session();
    _exit(128 + signal_number);
}

static int parse_port(const char *text, uint16_t *port)
{
    char *end = NULL;
    long value;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > 65535)
        return -1;
    *port = (uint16_t)value;
    return 0;
}

int main(int argc, char **argv)
{
    struct ngcd_app app;
    const char *address = "127.0.0.1";
    uint16_t port = 8989;
    bool mock = false;
    bool probe_target = false;
    bool probe_wifi = false;
    bool probe_storage = false;
    int index;

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--bind") == 0 && index + 1 < argc)
            address = argv[++index];
        else if (strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            if (parse_port(argv[++index], &port) != 0) {
                fprintf(stderr, "ngcd: invalid port\n");
                return 2;
            }
        } else if (strcmp(argv[index], "--mock") == 0)
            mock = true;
        else if (strcmp(argv[index], "--probe-target") == 0)
            probe_target = true;
        else if (strcmp(argv[index], "--probe-wifi") == 0)
            probe_wifi = true;
        else if (strcmp(argv[index], "--probe-storage") == 0)
            probe_storage = true;
        else {
            fprintf(stderr, "usage: %s [--bind ADDRESS] [--port PORT] "
                            "[--mock] [--probe-target] [--probe-wifi] "
                            "[--probe-storage]\n",
                    argv[0]);
            return 2;
        }
    }

    ngcd_app_init(&app);
    ngcd_app_load_product_identity(&app);
    if (probe_wifi) {
        struct ngcd_wifi_info info;
        struct ngcd_wifi_network networks[NGCD_WIFI_NETWORKS_MAX];
        size_t count;
        size_t network;
        if (ngcd_wifi_read_status(&info) != 0 ||
            ngcd_wifi_scan(networks, NGCD_WIFI_NETWORKS_MAX, &count) != 0) {
            fprintf(stderr, "ngcd: WLAN probe failed\n");
            return 1;
        }
        printf("wifi ip=%s mac=%s ssid=%s quality=%d level=%d\n",
               info.ip_address, info.mac_address, info.ssid,
               info.quality, info.level);
        for (network = 0; network < count; ++network)
            printf("network quality=%d level=%d ssid=%s\n",
                   networks[network].quality, networks[network].level,
                   networks[network].ssid);
        return 0;
    }
    if (probe_storage) {
        struct ngcd_storage_info info;
        if (ngcd_storage_read_status(&info) != 0) {
            fprintf(stderr, "ngcd: storage probe failed\n");
            return 1;
        }
        printf("storage location=%s total_bytes=%llu free_bytes=%llu\n",
               info.location,
               (unsigned long long)info.total_bytes,
               (unsigned long long)info.free_bytes);
        return 0;
    }
    if (probe_target) {
        struct ngcd_rk_target *target = NULL;
        struct ngcd_rk_api api;
        if (ngcd_rk_target_open(&target, &api) != 0) {
            fprintf(stderr, "ngcd: Rockchip ABI probe failed\n");
            return 1;
        }
        ngcd_rk_target_close(target);
        puts("ngcd: Rockchip ABI probe passed");
        return 0;
    }
    app.backend.ops = mock ? ngcd_mock_backend_ops() : ngcd_target_backend_ops();
    {
        struct sigaction action;
        struct sigaction fatal;
        struct sigaction ignore;
        memset(&action, 0, sizeof(action));
        memset(&fatal, 0, sizeof(fatal));
        memset(&ignore, 0, sizeof(ignore));
        action.sa_handler = handle_signal;
        fatal.sa_handler = handle_fatal_signal;
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&action.sa_mask);
        sigemptyset(&fatal.sa_mask);
        sigemptyset(&ignore.sa_mask);
        if (sigaction(SIGINT, &action, NULL) != 0 ||
            sigaction(SIGTERM, &action, NULL) != 0 ||
            sigaction(SIGILL, &fatal, NULL) != 0 ||
            sigaction(SIGABRT, &fatal, NULL) != 0 ||
            sigaction(SIGFPE, &fatal, NULL) != 0 ||
            sigaction(SIGSEGV, &fatal, NULL) != 0 ||
            sigaction(SIGBUS, &fatal, NULL) != 0 ||
            sigaction(SIGPIPE, &ignore, NULL) != 0) {
            fprintf(stderr, "ngcd: could not install signal handlers\n");
            (void)ngcd_select_stock_session();
            return 1;
        }
    }
    if (app.backend.ops->start(&app.backend) != 0) {
        fprintf(stderr, "ngcd: backend initialization failed\n");
        (void)ngcd_select_stock_session();
        return 1;
    }
    if (ngcd_serve(&app, address, port) != 0) {
        perror("ngcd");
        app.backend.ops->stop(&app.backend);
        (void)ngcd_select_stock_session();
        return 1;
    }
    app.backend.ops->stop(&app.backend);
    if (app.poweroff_requested) {
        if (ngcd_system_poweroff() != 0) {
            perror("ngcd: poweroff");
            (void)ngcd_select_stock_session();
            return 1;
        }
    }
    return 0;
}
