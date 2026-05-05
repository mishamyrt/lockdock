#ifndef LOCKDOCKD_LAUNCHAGENT_H
#define LOCKDOCKD_LAUNCHAGENT_H

#include <stdbool.h>
#include <stddef.h>

#define LOCKDOCK_LAUNCHAGENT_MESSAGE_SIZE 1024

bool lockdock_launchagent_enable(char *message, size_t message_size);
bool lockdock_launchagent_disable(char *message, size_t message_size);

#endif
