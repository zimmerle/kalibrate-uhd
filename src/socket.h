#ifndef SOCKET_TEST_H
#define SOCKET_TEST_H

#include <stdlib.h>
#include <stdint.h>

int make_connection(char *addr, uint16_t port);
int setup_server(uint16_t port);
ssize_t writen(int fd, const char *buf, size_t num);
ssize_t readn(int fd, char *buf, size_t num);

#endif /* SOCKET_TEST_H */

