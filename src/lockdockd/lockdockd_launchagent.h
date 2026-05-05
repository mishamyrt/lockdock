#ifndef LOCKDOCKD_LAUNCHAGENT_H
#define LOCKDOCKD_LAUNCHAGENT_H

#include <stdbool.h>
#include <stddef.h>

#define LOCKDOCKD_LAUNCHAGENT_MESSAGE_SIZE 1024

bool lockdockd_launchagent_enable(char *message, size_t message_size);
bool lockdockd_launchagent_disable(char *message, size_t message_size);

#endif
