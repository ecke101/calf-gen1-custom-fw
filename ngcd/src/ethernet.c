#include "ngcd.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

int ngcd_network_interface_ipv4(const char *interface_name,
                                char *address, size_t address_size)
{
    struct ifaddrs *addresses = NULL;
    struct ifaddrs *entry;
    int result = -1;
    if (interface_name == NULL || address == NULL || address_size < 8U)
        return -1;
    address[0] = '\0';
    if (getifaddrs(&addresses) != 0)
        return -1;
    for (entry = addresses; entry != NULL; entry = entry->ifa_next) {
        const struct sockaddr_in *ipv4;
        if (entry->ifa_addr == NULL || entry->ifa_name == NULL ||
            strcmp(entry->ifa_name, interface_name) != 0 ||
            entry->ifa_addr->sa_family != AF_INET)
            continue;
        ipv4 = (const struct sockaddr_in *)entry->ifa_addr;
        if (inet_ntop(AF_INET, &ipv4->sin_addr, address,
                      (socklen_t)address_size) != NULL) {
            result = 0;
            break;
        }
    }
    freeifaddrs(addresses);
    return result;
}

int ngcd_ethernet_read_status(struct ngcd_ethernet_info *info)
{
    if (info == NULL)
        return -1;
    memset(info, 0, sizeof(*info));
    if (ngcd_network_interface_ipv4("eth0", info->ip_address,
                                    sizeof(info->ip_address)) != 0)
        memcpy(info->ip_address, "0.0.0.0", sizeof("0.0.0.0"));
    return 0;
}
