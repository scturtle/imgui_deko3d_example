#pragma once

#include <cstdio>
#include <switch.h>

#define MSGBOX(fmt, ...)                                                          \
  do {                                                                         \
    char buf[256];                                                             \
    snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__);                            \
    ErrorApplicationConfig c;                                                  \
    errorApplicationCreate(&c, buf, 0);                                        \
    errorApplicationSetNumber(&c, 0);                                          \
    errorApplicationShow(&c);                                                  \
  } while (0)
