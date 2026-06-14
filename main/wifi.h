#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>
#include <stddef.h>

bool wifi_init_sta(void);
bool wifi_is_connected(void);
bool wifi_get_ip_address(char *out_ip, size_t out_size);

#endif
