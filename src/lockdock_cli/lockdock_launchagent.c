#include "lockdock_launchagent.h"

#include <lockdock_ipc.h>

#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define LOCKDOCK_LAUNCHAGENT_LABEL LOCKDOCK_IPC_BUNDLE_ID

enum {
    LOCKDOCK_LAUNCHAGENT_PERMISSION_DIR = 0755,
    LOCKDOCK_LAUNCHAGENT_PERMISSION_FILE = 0644,
    LOCKDOCK_LAUNCHAGENT_CONTENT_MAX = 4096,
    LOCKDOCK_LAUNCHAGENT_DOMAIN_TARGET_MAX = 64,
    LOCKDOCK_LAUNCHAGENT_SERVICE_TARGET_MAX = 96,
    LOCKDOCK_MAX_OUTPUT_SIZE = 256,
};

static void lockdock_set_message(char *buffer,
                                 size_t buffer_size,
                                 const char *message) {
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    snprintf(buffer, buffer_size, "%s", message);
}

static bool lockdock_append_bytes(char *buffer,
                                  size_t buffer_size,
                                  size_t *used,
                                  const char *data,
                                  size_t data_size) {
    if (buffer == NULL || used == NULL || data == NULL) {
        return false;
    }

    if (*used + data_size >= buffer_size) {
        return false;
    }

    memcpy(buffer + *used, data, data_size);
    *used += data_size;
    buffer[*used] = '\0';
    return true;
}

static bool lockdock_append_cstring(char *buffer,
                                    size_t buffer_size,
                                    size_t *used,
                                    const char *text) {
    if (text == NULL) {
        return false;
    }

    return lockdock_append_bytes(buffer, buffer_size, used, text, strlen(text));
}

static bool lockdock_append_xml_escaped(char *buffer,
                                        size_t buffer_size,
                                        size_t *used,
                                        const char *text) {
    while (text != NULL && *text != '\0') {
        const char *replacement = NULL;

        switch (*text) {
            case '&':
                replacement = "&amp;";
                break;
            case '<':
                replacement = "&lt;";
                break;
            case '>':
                replacement = "&gt;";
                break;
            case '"':
                replacement = "&quot;";
                break;
            case '\'':
                replacement = "&apos;";
                break;
            default:
                break;
        }

        if (replacement != NULL) {
            if (!lockdock_append_cstring(buffer, buffer_size, used, replacement)) {
                return false;
            }
        } else if (!lockdock_append_bytes(buffer, buffer_size, used, text, 1)) {
            return false;
        }

        text++;
    }

    return true;
}

static void lockdock_trim_trailing_whitespace(char *text) {
    size_t length;

    if (text == NULL) {
        return;
    }

    length = strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r' ||
                          text[length - 1] == ' ' || text[length - 1] == '\t')) {
        text[--length] = '\0';
    }
}

static const char *lockdock_home_dir(void) {
    const char *home = getenv("HOME");

    if (home != NULL && home[0] != '\0') {
        return home;
    }

    {
        struct passwd *pwd = getpwuid(getuid());

        if (pwd != NULL && pwd->pw_dir != NULL && pwd->pw_dir[0] != '\0') {
            return pwd->pw_dir;
        }
    }

    return NULL;
}

static bool lockdock_copy_launchagents_dir(char *buffer,
                                           size_t buffer_size,
                                           char *error,
                                           size_t error_size) {
    const char *home = lockdock_home_dir();

    if (home == NULL) {
        lockdock_set_message(error, error_size,
                             "Could not determine the current home directory");
        return false;
    }

    if (snprintf(buffer, buffer_size, "%s/Library/LaunchAgents", home) >=
        (int)buffer_size) {
        lockdock_set_message(error, error_size,
                             "LaunchAgents directory path is too long");
        return false;
    }

    return true;
}

static bool lockdock_copy_plist_path(char *buffer,
                                     size_t buffer_size,
                                     char *error,
                                     size_t error_size) {
    char directory[PATH_MAX];

    if (!lockdock_copy_launchagents_dir(directory, sizeof(directory), error,
                                        error_size)) {
        return false;
    }

    if (snprintf(buffer, buffer_size, "%s/%s.plist", directory,
                 LOCKDOCK_LAUNCHAGENT_LABEL) >= (int)buffer_size) {
        lockdock_set_message(error, error_size,
                             "LaunchAgent plist path is too long");
        return false;
    }

    return true;
}

static bool lockdock_mkdir_p(const char *path, char *error, size_t error_size) {
    char tmp[PATH_MAX];
    size_t length;

    if (path == NULL || path[0] == '\0') {
        lockdock_set_message(error, error_size,
                             "LaunchAgents directory path is empty");
        return false;
    }

    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) {
        lockdock_set_message(error, error_size,
                             "LaunchAgents directory path is too long");
        return false;
    }

    length = strlen(tmp);
    for (size_t i = 1; i < length; i++) {
        if (tmp[i] != '/') {
            continue;
        }

        tmp[i] = '\0';
        if (mkdir(tmp, LOCKDOCK_LAUNCHAGENT_PERMISSION_DIR) != 0 &&
            errno != EEXIST) {
            snprintf(error, error_size, "Failed to create directory '%s': %s", tmp,
                     strerror(errno));
            return false;
        }
        tmp[i] = '/';
    }

    if (mkdir(tmp, LOCKDOCK_LAUNCHAGENT_PERMISSION_DIR) != 0 && errno != EEXIST) {
        snprintf(error, error_size, "Failed to create directory '%s': %s", tmp,
                 strerror(errno));
        return false;
    }

    return true;
}

static bool lockdock_copy_executable_path(char *buffer,
                                          size_t buffer_size,
                                          char *error,
                                          size_t error_size) {
    uint32_t raw_size = (uint32_t)buffer_size;
    char raw_path[PATH_MAX];
    char *resolved;

    if (_NSGetExecutablePath(raw_path, &raw_size) != 0) {
        lockdock_set_message(error, error_size, "Executable path is too long");
        return false;
    }

    resolved = realpath(raw_path, buffer);
    if (resolved == NULL) {
        snprintf(error, error_size, "Failed to resolve executable path: %s",
                 strerror(errno));
        return false;
    }

    return true;
}

static bool lockdock_copy_daemon_path(char *buffer,
                                      size_t buffer_size,
                                      char *error,
                                      size_t error_size) {
    char executable_path[PATH_MAX];

    if (!lockdock_copy_executable_path(executable_path, sizeof(executable_path),
                                       error, error_size)) {
        return false;
    }

    if (snprintf(buffer, buffer_size, "%sd", executable_path) >= (int)buffer_size) {
        lockdock_set_message(error, error_size, "Daemon path is too long");
        return false;
    }

    if (access(buffer, X_OK) != 0) {
        snprintf(error, error_size,
                 "Expected daemon binary at %s, but it is missing or not executable",
                 buffer);
        return false;
    }

    return true;
}

static bool lockdock_copy_domain_target(char *buffer,
                                        size_t buffer_size,
                                        char *error,
                                        size_t error_size) {
    if (snprintf(buffer, buffer_size, "gui/%u", (unsigned)getuid()) >=
        (int)buffer_size) {
        lockdock_set_message(error, error_size,
                             "launchctl domain target is too long");
        return false;
    }

    return true;
}

static bool lockdock_copy_service_target(char *buffer,
                                         size_t buffer_size,
                                         char *error,
                                         size_t error_size) {
    if (snprintf(buffer, buffer_size, "gui/%u/%s", (unsigned)getuid(),
                 LOCKDOCK_LAUNCHAGENT_LABEL) >= (int)buffer_size) {
        lockdock_set_message(error, error_size,
                             "launchctl service target is too long");
        return false;
    }

    return true;
}

static bool lockdock_build_plist(char *buffer,
                                 size_t buffer_size,
                                 const char *executable_path,
                                 char *error,
                                 size_t error_size) {
    size_t used = 0;

    if (buffer == NULL || executable_path == NULL) {
        lockdock_set_message(error, error_size, "Internal error");
        return false;
    }

    buffer[0] = '\0';

    if (!lockdock_append_cstring(
            buffer, buffer_size, &used,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
            "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\">\n"
            "<dict>\n"
            "    <key>Label</key>\n"
            "    <string>") ||
        !lockdock_append_xml_escaped(buffer, buffer_size, &used,
                                     LOCKDOCK_LAUNCHAGENT_LABEL) ||
        !lockdock_append_cstring(buffer, buffer_size, &used,
                                 "</string>\n"
                                 "    <key>ProgramArguments</key>\n"
                                 "    <array>\n"
                                 "        <string>") ||
        !lockdock_append_xml_escaped(buffer, buffer_size, &used, executable_path) ||
        !lockdock_append_cstring(buffer, buffer_size, &used,
                                 "</string>\n"
                                 "    </array>\n"
                                 "    <key>RunAtLoad</key>\n"
                                 "    <true/>\n"
                                 "    <key>KeepAlive</key>\n"
                                 "    <true/>\n"
                                 "</dict>\n"
                                 "</plist>\n")) {
        lockdock_set_message(error, error_size, "LaunchAgent plist is too large");
        return false;
    }

    return true;
}

static bool lockdock_write_all(int fd,
                               const char *buffer,
                               size_t length,
                               char *error,
                               size_t error_size) {
    size_t written_total = 0;

    while (written_total < length) {
        ssize_t written = write(fd, buffer + written_total, length - written_total);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }

            snprintf(error, error_size, "Failed to write LaunchAgent plist: %s",
                     strerror(errno));
            return false;
        }

        written_total += (size_t)written;
    }

    return true;
}

static bool lockdock_write_plist(const char *plist_path,
                                 const char *plist_content,
                                 char *error,
                                 size_t error_size) {
    char temporary_path[PATH_MAX];
    int fd = -1;
    bool success = false;

    if (plist_path == NULL || plist_content == NULL) {
        lockdock_set_message(error, error_size, "Internal error");
        return false;
    }

    if (snprintf(temporary_path, sizeof(temporary_path), "%s.XXXXXX", plist_path) >=
        (int)sizeof(temporary_path)) {
        lockdock_set_message(error, error_size,
                             "Temporary LaunchAgent plist path is too long");
        return false;
    }

    fd = mkstemp(temporary_path);
    if (fd < 0) {
        snprintf(error, error_size, "Failed to create LaunchAgent plist: %s",
                 strerror(errno));
        return false;
    }

    if (fchmod(fd, LOCKDOCK_LAUNCHAGENT_PERMISSION_FILE) != 0) {
        snprintf(error, error_size, "Failed to set LaunchAgent plist mode: %s",
                 strerror(errno));
        goto cleanup;
    }

    if (!lockdock_write_all(fd, plist_content, strlen(plist_content), error,
                            error_size)) {
        goto cleanup;
    }

    if (fsync(fd) != 0) {
        snprintf(error, error_size, "Failed to flush LaunchAgent plist: %s",
                 strerror(errno));
        goto cleanup;
    }

    if (close(fd) != 0) {
        fd = -1;
        snprintf(error, error_size, "Failed to close LaunchAgent plist: %s",
                 strerror(errno));
        goto cleanup;
    }
    fd = -1;

    if (rename(temporary_path, plist_path) != 0) {
        snprintf(error, error_size, "Failed to install LaunchAgent plist: %s",
                 strerror(errno));
        goto cleanup;
    }

    success = true;

cleanup:
    if (fd >= 0) {
        close(fd);
    }

    if (!success) {
        unlink(temporary_path);
    }

    return success;
}

static bool lockdock_read_command_output(int fd, char *buffer, size_t buffer_size) {
    size_t used = 0;

    if (buffer != NULL && buffer_size > 0) {
        buffer[0] = '\0';
    }

    while (1) {
        char chunk[LOCKDOCK_MAX_OUTPUT_SIZE];
        ssize_t nread = read(fd, chunk, sizeof(chunk));

        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        if (nread == 0) {
            break;
        }

        if (buffer != NULL && buffer_size > 1 && used < buffer_size - 1) {
            size_t available = buffer_size - used - 1;
            size_t copy_size = (size_t)nread;

            if (copy_size > available) {
                copy_size = available;
            }

            memcpy(buffer + used, chunk, copy_size);
            used += copy_size;
            buffer[used] = '\0';
        }
    }

    return true;
}

static bool lockdock_run_command(const char *const *argv,
                                 int *exit_status,
                                 char *output,
                                 size_t output_size,
                                 char *error,
                                 size_t error_size) {
    int pipefd[2];
    pid_t pid;
    int status = 0;

    if (argv == NULL || argv[0] == NULL || exit_status == NULL) {
        lockdock_set_message(error, error_size, "Internal error");
        return false;
    }

    if (pipe(pipefd) != 0) {
        snprintf(error, error_size, "Failed to create subprocess pipe: %s",
                 strerror(errno));
        return false;
    }

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        snprintf(error, error_size, "Failed to fork subprocess: %s",
                 strerror(errno));
        return false;
    }

    if (pid == 0) {
        close(pipefd[0]);

        if (dup2(pipefd[1], STDOUT_FILENO) < 0 ||
            dup2(pipefd[1], STDERR_FILENO) < 0) {
            fprintf(stderr, "Failed to redirect subprocess output: %s\n",
                    strerror(errno));
            _exit(127);  // NOLINT
        }

        close(pipefd[1]);
        execvp(argv[0], (char *const *)argv);
        fprintf(stderr, "Failed to exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);  // NOLINT
    }

    close(pipefd[1]);

    if (!lockdock_read_command_output(pipefd[0], output, output_size)) {
        close(pipefd[0]);
        snprintf(error, error_size, "Failed to read subprocess output: %s",
                 strerror(errno));
        waitpid(pid, NULL, 0);
        return false;
    }

    close(pipefd[0]);

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            snprintf(error, error_size, "Failed to wait for subprocess: %s",
                     strerror(errno));
            return false;
        }
    }

    if (output != NULL && output_size > 0) {
        lockdock_trim_trailing_whitespace(output);
    }

    if (WIFEXITED(status)) {
        *exit_status = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        *exit_status = 128 + WTERMSIG(status);  // NOLINT
    } else {
        *exit_status = 1;
    }

    return true;
}

static bool lockdock_launchctl(char *error,
                               size_t error_size,
                               const char *action,
                               const char *const *argv) {
    char output[LOCKDOCK_LAUNCHAGENT_MESSAGE_SIZE];
    int exit_status = 0;

    if (!lockdock_run_command(argv, &exit_status, output, sizeof(output), error,
                              error_size)) {
        return false;
    }

    if (exit_status == 0) {
        return true;
    }

    if (output[0] != '\0') {
        snprintf(error, error_size, "%s failed: %s", action, output);
    } else {
        snprintf(error, error_size, "%s failed with exit status %d", action,
                 exit_status);
    }

    return false;
}

static bool lockdock_launchctl_best_effort(const char *const *argv,
                                           char *error,
                                           size_t error_size) {
    char output[LOCKDOCK_LAUNCHAGENT_MESSAGE_SIZE];
    int exit_status = 0;

    if (!lockdock_run_command(argv, &exit_status, output, sizeof(output), error,
                              error_size)) {
        return false;
    }

    return true;
}

bool lockdock_launchagent_enable(char *message, size_t message_size) {
    char daemon_path[PATH_MAX];
    char directory[PATH_MAX];
    char plist_path[PATH_MAX];
    char plist_content[LOCKDOCK_LAUNCHAGENT_CONTENT_MAX];
    char domain_target[LOCKDOCK_LAUNCHAGENT_DOMAIN_TARGET_MAX];
    char service_target[LOCKDOCK_LAUNCHAGENT_SERVICE_TARGET_MAX];
    char error[LOCKDOCK_LAUNCHAGENT_MESSAGE_SIZE];
    const char *enable_argv[] = {"launchctl", "enable", service_target, NULL};
    const char *bootout_argv[] = {"launchctl", "bootout", service_target, NULL};
    const char *bootstrap_argv[] = {"launchctl", "bootstrap", domain_target,
                                    plist_path, NULL};

    if (!lockdock_copy_daemon_path(daemon_path, sizeof(daemon_path), error,
                                   sizeof(error)) ||
        !lockdock_copy_launchagents_dir(directory, sizeof(directory), error,
                                        sizeof(error)) ||
        !lockdock_copy_plist_path(plist_path, sizeof(plist_path), error,
                                  sizeof(error)) ||
        !lockdock_copy_domain_target(domain_target, sizeof(domain_target), error,
                                     sizeof(error)) ||
        !lockdock_copy_service_target(service_target, sizeof(service_target), error,
                                      sizeof(error))) {
        lockdock_set_message(message, message_size, error);
        return false;
    }

    if (!lockdock_mkdir_p(directory, error, sizeof(error)) ||
        !lockdock_build_plist(plist_content, sizeof(plist_content), daemon_path,
                              error, sizeof(error)) ||
        !lockdock_write_plist(plist_path, plist_content, error, sizeof(error))) {
        lockdock_set_message(message, message_size, error);
        return false;
    }

    if (!lockdock_launchctl_best_effort(enable_argv, error, sizeof(error)) ||
        !lockdock_launchctl_best_effort(bootout_argv, error, sizeof(error)) ||
        !lockdock_launchctl(error, sizeof(error), "launchctl bootstrap",
                            bootstrap_argv)) {
        lockdock_set_message(message, message_size, error);
        return false;
    }

    snprintf(message, message_size, "Enabled LaunchAgent at %s", plist_path);
    return true;
}

bool lockdock_launchagent_disable(char *message, size_t message_size) {
    char plist_path[PATH_MAX];
    char service_target[LOCKDOCK_LAUNCHAGENT_SERVICE_TARGET_MAX];
    char error[LOCKDOCK_LAUNCHAGENT_MESSAGE_SIZE];
    const char *disable_argv[] = {"launchctl", "disable", service_target, NULL};
    const char *bootout_argv[] = {"launchctl", "bootout", service_target, NULL};

    if (!lockdock_copy_plist_path(plist_path, sizeof(plist_path), error,
                                  sizeof(error)) ||
        !lockdock_copy_service_target(service_target, sizeof(service_target), error,
                                      sizeof(error))) {
        lockdock_set_message(message, message_size, error);
        return false;
    }

    if (!lockdock_launchctl(error, sizeof(error), "launchctl disable",
                            disable_argv) ||
        !lockdock_launchctl_best_effort(bootout_argv, error, sizeof(error))) {
        lockdock_set_message(message, message_size, error);
        return false;
    }

    if (unlink(plist_path) != 0 && errno != ENOENT) {
        snprintf(error, sizeof(error), "Failed to remove LaunchAgent plist: %s",
                 strerror(errno));
        lockdock_set_message(message, message_size, error);
        return false;
    }

    snprintf(message, message_size, "Disabled LaunchAgent and removed %s",
             plist_path);
    return true;
}
