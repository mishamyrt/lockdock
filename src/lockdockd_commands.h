#ifndef LOCKDOCKD_COMMANDS_H
#define LOCKDOCKD_COMMANDS_H

int lockdockd_cmd_daemon(void);
int lockdockd_cmd_enable(void);
int lockdockd_cmd_disable(void);
void lockdockd_print_usage(const char *prog);

#endif
