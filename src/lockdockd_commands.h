#ifndef LOCKDOCKD_COMMANDS_H
#define LOCKDOCKD_COMMANDS_H

int lockdockd_cmd_daemon(void);
int lockdockd_cmd_status(void);
int lockdockd_cmd_list(void);
int lockdockd_cmd_relocate(const char *display_arg);
int lockdockd_cmd_lock(const char *display_arg);
int lockdockd_cmd_unlock(void);
void lockdockd_print_usage(const char *prog);

#endif
