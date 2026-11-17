#ifndef PORTHOLE_TOUCH_WDA_NET_H
#define PORTHOLE_TOUCH_WDA_NET_H

/* Optional TCP transport for the WebDriverAgent client. */

#include "touch/wda.h"

typedef struct ph_wda_tcp ph_wda_tcp;

/* Connects and installs the socket as the client's send callback. NULL on failure. */
ph_wda_tcp *ph_wda_tcp_open(ph_wda *wda, const char *host, int port);
void ph_wda_tcp_close(ph_wda_tcp *tcp);

#endif
