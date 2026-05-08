#include "lockdock_ipc.h"

#include "lockdock_config.h"

#include <json.h>

#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

enum {
    LOCKDOCK_IPC_DECIMAL_BASE = 10,
    LOCKDOCK_IPC_FIELD_TEXT_SIZE = 64,
};

static void lockdock_ipc_set_error(char *buffer,
                                   size_t buffer_size,
                                   const char *message) {
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    snprintf(buffer, buffer_size, "%s", message);
}

static const char *lockdock_ipc_home_dir(void) {
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

static bool lockdock_ipc_copy_socket_dir(char *buffer,
                                         size_t buffer_size,
                                         char *error,
                                         size_t error_size) {
    const char *home = lockdock_ipc_home_dir();

    if (home == NULL) {
        lockdock_ipc_set_error(error, error_size,
                               "Could not determine the current home directory");
        return false;
    }

    if (snprintf(buffer, buffer_size, "%s/Library/Caches/%s", home,
                 LOCKDOCK_BUNDLE_ID) >= (int)buffer_size) {
        lockdock_ipc_set_error(error, error_size,
                               "Socket directory path is too long");
        return false;
    }

    return true;
}

static bool lockdock_ipc_mkdir_p(const char *path,
                                 mode_t mode,
                                 char *error,
                                 size_t error_size) {
    char tmp[PATH_MAX];

    if (path == NULL || path[0] == '\0') {
        lockdock_ipc_set_error(error, error_size, "Directory path is empty");
        return false;
    }

    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) {
        lockdock_ipc_set_error(error, error_size, "Directory path is too long");
        return false;
    }

    size_t length = strlen(tmp);
    for (size_t i = 1; i < length; i++) {
        if (tmp[i] != '/') {
            continue;
        }

        tmp[i] = '\0';
        if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
            snprintf(error, error_size, "Failed to create directory '%s': %s", tmp,
                     strerror(errno));
            return false;
        }
        tmp[i] = '/';
    }

    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        snprintf(error, error_size, "Failed to create directory '%s': %s", tmp,
                 strerror(errno));
        return false;
    }

    return true;
}

static bool lockdock_ipc_append_bytes(char *buffer,
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

static bool lockdock_ipc_append_format(char *buffer,
                                       size_t buffer_size,
                                       size_t *used,
                                       const char *format,
                                       ...) {
    va_list args;

    if (buffer == NULL || used == NULL || format == NULL) {
        return false;
    }

    va_start(args, format);
    int written = vsnprintf(buffer + *used, buffer_size - *used, format, args);
    va_end(args);

    if (written < 0 || *used + (size_t)written >= buffer_size) {
        return false;
    }

    *used += (size_t)written;
    return true;
}

static bool lockdock_ipc_append_json_string(char *buffer,
                                            size_t buffer_size,
                                            size_t *used,
                                            const char *text) {
    const unsigned char *cursor = (const unsigned char *)text;

    if (!lockdock_ipc_append_bytes(buffer, buffer_size, used, "\"", 1)) {
        return false;
    }

    while (cursor != NULL && *cursor != '\0') {
        char escaped[sizeof("\\u001f")];
        size_t escaped_size = 0;

        if (*cursor == '"' || *cursor == '\\') {
            escaped[0] = '\\';
            escaped[1] = (char)*cursor;
            escaped_size = 2;
        } else if (*cursor == '\n') {
            escaped[0] = '\\';
            escaped[1] = 'n';
            escaped_size = 2;
        } else if (*cursor == '\r') {
            escaped[0] = '\\';
            escaped[1] = 'r';
            escaped_size = 2;
        } else if (*cursor == '\t') {
            escaped[0] = '\\';
            escaped[1] = 't';
            escaped_size = 2;
        } else if (*cursor < ' ') {
            snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
            escaped_size = strlen(escaped);
        } else {
            escaped[0] = (char)*cursor;
            escaped_size = 1;
        }

        if (!lockdock_ipc_append_bytes(buffer, buffer_size, used, escaped,
                                       escaped_size)) {
            return false;
        }

        cursor++;
    }

    return lockdock_ipc_append_bytes(buffer, buffer_size, used, "\"", 1);
}

static void lockdock_ipc_trim_trailing_whitespace(char *text) {
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

static bool lockdock_ipc_json_name_equals(const json_string_t *name,
                                          const char *text) {
    size_t text_length;

    if (name == NULL || name->string == NULL || text == NULL) {
        return false;
    }

    text_length = strlen(text);
    return name->string_size == text_length &&
           memcmp(name->string, text, text_length) == 0;
}

static void lockdock_ipc_set_json_parse_error(const char *kind,
                                              const json_parse_result_t *result,
                                              char *error,
                                              size_t error_size) {
    const char *label = kind == NULL ? "message" : kind;

    if (result == NULL || result->error == json_parse_error_none) {
        snprintf(error, error_size, "Invalid JSON %s", label);
        return;
    }

    snprintf(error, error_size, "Invalid JSON %s at byte %zu", label,
             result->error_offset);
}

static bool lockdock_ipc_parse_json_object(const char *json_text,
                                           const char *kind,
                                           json_object_t **object_out,
                                           json_value_t **root_out,
                                           char *error,
                                           size_t error_size) {
    json_parse_result_t parse_result;
    json_value_t *root = NULL;

    if (json_text == NULL || object_out == NULL || root_out == NULL) {
        lockdock_ipc_set_error(error, error_size, "Internal error");
        return false;
    }

    memset(&parse_result, 0, sizeof(parse_result));
    root = json_parse_ex(json_text, strlen(json_text), json_parse_flags_default,
                         NULL, NULL, &parse_result);
    if (root == NULL) {
        lockdock_ipc_set_json_parse_error(kind, &parse_result, error, error_size);
        return false;
    }

    if (root->type != json_type_object) {
        snprintf(error, error_size, "%s must be a single JSON object",
                 kind == NULL ? "Message" : kind);
        free(root);
        return false;
    }

    *object_out = json_value_as_object(root);
    *root_out = root;
    return *object_out != NULL;
}

static bool lockdock_ipc_json_copy_string(const json_value_t *value,
                                          char *buffer,
                                          size_t buffer_size,
                                          const char *field_name,
                                          char *error,
                                          size_t error_size) {
    json_string_t *string_value;

    if (value == NULL || buffer == NULL || field_name == NULL) {
        lockdock_ipc_set_error(error, error_size, "Internal error");
        return false;
    }

    if (value->type != json_type_string) {
        snprintf(error, error_size, "Field '%s' must be a string", field_name);
        return false;
    }

    string_value = json_value_as_string((json_value_t *)value);
    if (string_value == NULL || string_value->string == NULL) {
        lockdock_ipc_set_error(error, error_size, "Internal error");
        return false;
    }

    if (string_value->string_size >= buffer_size) {
        snprintf(error, error_size, "Field '%s' is too long", field_name);
        return false;
    }

    memcpy(buffer, string_value->string, string_value->string_size);
    buffer[string_value->string_size] = '\0';
    return true;
}

static bool lockdock_ipc_json_copy_number_text(const json_value_t *value,
                                               char *buffer,
                                               size_t buffer_size,
                                               const char *field_name,
                                               char *error,
                                               size_t error_size) {
    json_number_t *number_value;

    if (value == NULL || buffer == NULL || field_name == NULL) {
        lockdock_ipc_set_error(error, error_size, "Internal error");
        return false;
    }

    if (value->type != json_type_number) {
        snprintf(error, error_size, "Field '%s' must be an integer", field_name);
        return false;
    }

    number_value = json_value_as_number((json_value_t *)value);
    if (number_value == NULL || number_value->number == NULL) {
        lockdock_ipc_set_error(error, error_size, "Internal error");
        return false;
    }

    if (number_value->number_size >= buffer_size) {
        snprintf(error, error_size, "Field '%s' is too long", field_name);
        return false;
    }

    memcpy(buffer, number_value->number, number_value->number_size);
    buffer[number_value->number_size] = '\0';
    return true;
}

static bool lockdock_ipc_parse_non_negative_int_text(const char *text,
                                                     int *value_out,
                                                     const char *field_name,
                                                     char *error,
                                                     size_t error_size) {
    char *endptr = NULL;

    if (text == NULL || value_out == NULL || field_name == NULL) {
        lockdock_ipc_set_error(error, error_size, "Internal error");
        return false;
    }

    if (strchr(text, '.') != NULL || strchr(text, 'e') != NULL ||
        strchr(text, 'E') != NULL) {
        snprintf(error, error_size, "Field '%s' must be an integer", field_name);
        return false;
    }

    if (text[0] == '-') {
        long long parsed = strtoll(text, &endptr, LOCKDOCK_IPC_DECIMAL_BASE);

        if (endptr == text || *endptr != '\0') {
            snprintf(error, error_size, "Field '%s' must be an integer", field_name);
            return false;
        }

        if (parsed < 0 || parsed > INT_MAX) {
            snprintf(error, error_size, "Field '%s' must be a non-negative integer",
                     field_name);
            return false;
        }

        *value_out = (int)parsed;
        return true;
    }

    {
        unsigned long long parsed =
            strtoull(text, &endptr, LOCKDOCK_IPC_DECIMAL_BASE);

        if (endptr == text || *endptr != '\0') {
            snprintf(error, error_size, "Field '%s' must be an integer", field_name);
            return false;
        }

        if (parsed > INT_MAX) {
            snprintf(error, error_size, "Field '%s' must be a non-negative integer",
                     field_name);
            return false;
        }

        *value_out = (int)parsed;
        return true;
    }
}

static bool lockdock_ipc_json_parse_non_negative_int(const json_value_t *value,
                                                     int *value_out,
                                                     const char *field_name,
                                                     char *error,
                                                     size_t error_size) {
    char number_text[LOCKDOCK_IPC_FIELD_TEXT_SIZE];

    if (!lockdock_ipc_json_copy_number_text(value, number_text, sizeof(number_text),
                                            field_name, error, error_size)) {
        return false;
    }

    return lockdock_ipc_parse_non_negative_int_text(number_text, value_out,
                                                    field_name, error, error_size);
}

static bool lockdock_ipc_command_to_string(LockDockIpcCommand command,
                                           const char **name_out) {
    if (name_out == NULL) {
        return false;
    }

    switch (command) {
        case LOCKDOCK_IPC_COMMAND_GET_STATE:
            *name_out = "get_state";
            return true;

        case LOCKDOCK_IPC_COMMAND_SET_STATE:
            *name_out = "set_state";
            return true;

        case LOCKDOCK_IPC_COMMAND_UNLOCK:
            *name_out = "unlock";
            return true;

        default:
            return false;
    }
}

static bool lockdock_ipc_parse_command_name(const char *name,
                                            LockDockIpcRequest *request,
                                            char *error,
                                            size_t error_size) {
    if (request->command != LOCKDOCK_IPC_COMMAND_NONE) {
        lockdock_ipc_set_error(error, error_size,
                               "Field 'cmd' must be specified once");
        return false;
    }

    if (strcmp(name, "get_state") == 0) {
        request->command = LOCKDOCK_IPC_COMMAND_GET_STATE;
        return true;
    }

    if (strcmp(name, "set_state") == 0) {
        request->command = LOCKDOCK_IPC_COMMAND_SET_STATE;
        return true;
    }

    if (strcmp(name, "unlock") == 0) {
        request->command = LOCKDOCK_IPC_COMMAND_UNLOCK;
        return true;
    }

    lockdock_ipc_set_error(error, error_size, "Unknown command");
    return false;
}

static bool lockdock_ipc_parse_request_field(const json_object_element_t *element,
                                             LockDockIpcRequest *request,
                                             char *error,
                                             size_t error_size) {
    char value_buffer[LOCKDOCK_IPC_FIELD_TEXT_SIZE];

    if (element == NULL || request == NULL) {
        lockdock_ipc_set_error(error, error_size, "Internal error");
        return false;
    }

    if (lockdock_ipc_json_name_equals(element->name, "cmd")) {
        if (!lockdock_ipc_json_copy_string(element->value, value_buffer,
                                           sizeof(value_buffer), "cmd", error,
                                           error_size)) {
            return false;
        }

        return lockdock_ipc_parse_command_name(value_buffer, request, error,
                                               error_size);
    }

    if (lockdock_ipc_json_name_equals(element->name, "target")) {
        if (request->has_target) {
            lockdock_ipc_set_error(error, error_size,
                                   "Field 'target' must be specified once");
            return false;
        }

        if (!lockdock_ipc_json_parse_non_negative_int(
                element->value, &request->target, "target", error, error_size)) {
            return false;
        }

        request->has_target = true;
        return true;
    }

    if (element->name == NULL || element->name->string == NULL) {
        lockdock_ipc_set_error(error, error_size, "Invalid JSON request");
        return false;
    }

    snprintf(error, error_size, "Unknown field '%.*s'",
             (int)element->name->string_size, element->name->string);
    return false;
}

static bool lockdock_ipc_validate_request(const LockDockIpcRequest *request,
                                          char *error,
                                          size_t error_size) {
    if (request == NULL) {
        lockdock_ipc_set_error(error, error_size, "Internal error");
        return false;
    }

    if (request->command == LOCKDOCK_IPC_COMMAND_NONE) {
        lockdock_ipc_set_error(error, error_size, "Missing required field 'cmd'");
        return false;
    }

    if (request->command == LOCKDOCK_IPC_COMMAND_SET_STATE) {
        if (!request->has_target) {
            lockdock_ipc_set_error(error, error_size,
                                   "Missing required field 'target'");
            return false;
        }
    } else if (request->has_target) {
        lockdock_ipc_set_error(
            error, error_size,
            "Field 'target' is only valid for command 'set_state'");
        return false;
    }

    return true;
}

static bool lockdock_ipc_validate_response(bool has_displays,
                                           bool has_location,
                                           bool has_target,
                                           bool has_success,
                                           bool has_reason,
                                           char *error,
                                           size_t error_size) {
    if (has_success) {
        if (has_displays || has_location || has_target) {
            lockdock_ipc_set_error(error, error_size,
                                   "Response mixes result and state fields");
            return false;
        }

        return true;
    }

    if (!has_displays) {
        lockdock_ipc_set_error(error, error_size,
                               "Missing required field 'displays'");
        return false;
    }

    if (!has_location) {
        lockdock_ipc_set_error(error, error_size,
                               "Missing required field 'location'");
        return false;
    }

    if (has_reason) {
        lockdock_ipc_set_error(error, error_size,
                               "Field 'reason' is only valid for result responses");
        return false;
    }

    return true;
}

static bool lockdock_ipc_parse_display_array(const json_value_t *value,
                                             LockDockIpcState *state,
                                             char *error,
                                             size_t error_size) {
    json_array_t *array_value;
    json_array_element_t *element;
    uint32_t count = 0;

    if (value == NULL || state == NULL) {
        lockdock_ipc_set_error(error, error_size, "Internal error");
        return false;
    }

    if (value->type != json_type_array) {
        lockdock_ipc_set_error(error, error_size,
                               "Field 'displays' must be an array of strings");
        return false;
    }

    array_value = json_value_as_array((json_value_t *)value);
    if (array_value == NULL) {
        lockdock_ipc_set_error(error, error_size, "Internal error");
        return false;
    }

    if (array_value->length > LOCKDOCK_IPC_MAX_DISPLAYS) {
        lockdock_ipc_set_error(error, error_size,
                               "Response contains too many displays");
        return false;
    }

    for (element = array_value->start; element != NULL; element = element->next) {
        if (count >= LOCKDOCK_IPC_MAX_DISPLAYS) {
            lockdock_ipc_set_error(error, error_size,
                                   "Response contains too many displays");
            return false;
        }

        if (!lockdock_ipc_json_copy_string(element->value, state->displays[count],
                                           sizeof(state->displays[count]),
                                           "displays", error, error_size)) {
            if (strcmp(error, "Field 'displays' is too long") == 0) {
                lockdock_ipc_set_error(error, error_size,
                                       "Display name in response is too long");
            }
            return false;
        }
        count++;
    }

    state->display_count = (uint8_t)count;
    return true;
}

static bool lockdock_ipc_parse_response_field(const json_object_element_t *element,
                                              LockDockIpcState *state,
                                              LockDockIpcResult *result,
                                              bool *has_displays,
                                              bool *has_location,
                                              bool *has_target,
                                              bool *has_success,
                                              bool *has_reason,
                                              char *error,
                                              size_t error_size) {
    if (element == NULL || state == NULL || result == NULL) {
        lockdock_ipc_set_error(error, error_size, "Internal error");
        return false;
    }

    if (lockdock_ipc_json_name_equals(element->name, "displays")) {
        if (*has_displays) {
            lockdock_ipc_set_error(error, error_size,
                                   "Field 'displays' must be specified once");
            return false;
        }

        if (!lockdock_ipc_parse_display_array(element->value, state, error,
                                              error_size)) {
            return false;
        }

        *has_displays = true;
        return true;
    }

    if (lockdock_ipc_json_name_equals(element->name, "location")) {
        if (*has_location) {
            lockdock_ipc_set_error(error, error_size,
                                   "Field 'location' must be specified once");
            return false;
        }

        if (!lockdock_ipc_json_parse_non_negative_int(
                element->value, &state->location_index, "location", error,
                error_size)) {
            return false;
        }

        *has_location = true;
        return true;
    }

    if (lockdock_ipc_json_name_equals(element->name, "target")) {
        if (*has_target) {
            lockdock_ipc_set_error(error, error_size,
                                   "Field 'target' must be specified once");
            return false;
        }

        if (!lockdock_ipc_json_parse_non_negative_int(
                element->value, &state->target_index, "target", error, error_size)) {
            return false;
        }

        *has_target = true;
        return true;
    }

    if (lockdock_ipc_json_name_equals(element->name, "success")) {
        if (*has_success) {
            lockdock_ipc_set_error(error, error_size,
                                   "Field 'success' must be specified once");
            return false;
        }

        if (element->value == NULL || (element->value->type != json_type_true &&
                                       element->value->type != json_type_false)) {
            lockdock_ipc_set_error(error, error_size,
                                   "Field 'success' must be a boolean");
            return false;
        }

        result->success = element->value->type == json_type_true;
        *has_success = true;
        return true;
    }

    if (lockdock_ipc_json_name_equals(element->name, "reason")) {
        if (*has_reason) {
            lockdock_ipc_set_error(error, error_size,
                                   "Field 'reason' must be specified once");
            return false;
        }

        if (!lockdock_ipc_json_copy_string(element->value, result->reason,
                                           sizeof(result->reason), "reason", error,
                                           error_size)) {
            return false;
        }

        *has_reason = true;
        return true;
    }

    if (element->name == NULL || element->name->string == NULL) {
        lockdock_ipc_set_error(error, error_size, "Invalid JSON response");
        return false;
    }

    snprintf(error, error_size, "Unknown field '%.*s'",
             (int)element->name->string_size, element->name->string);
    return false;
}

static bool lockdock_ipc_write_all(int fd,
                                   const char *buffer,
                                   size_t length,
                                   char *error,
                                   size_t error_size) {
    size_t offset = 0;

    while (offset < length) {
        ssize_t written = write(fd, buffer + offset, length - offset);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }

            snprintf(error, error_size, "Failed to write daemon request: %s",
                     strerror(errno));
            return false;
        }

        offset += (size_t)written;
    }

    return true;
}

static bool lockdock_ipc_read_message(int fd,
                                      char *buffer,
                                      size_t buffer_size,
                                      const char *kind,
                                      char *error,
                                      size_t error_size) {
    size_t used = 0;

    if (buffer == NULL || kind == NULL) {
        lockdock_ipc_set_error(error, error_size, "Internal error");
        return false;
    }

    while (used + 1 < buffer_size) {
        ssize_t nread = read(fd, buffer + used, buffer_size - used - 1);

        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }

            snprintf(error, error_size, "Failed to read daemon %s: %s", kind,
                     strerror(errno));
            return false;
        }

        if (nread == 0) {
            break;
        }

        used += (size_t)nread;
    }

    if (used == 0) {
        snprintf(error, error_size, "Daemon sent an empty %s", kind);
        return false;
    }

    if (used + 1 >= buffer_size) {
        snprintf(error, error_size, "Daemon %s exceeded buffer", kind);
        return false;
    }

    buffer[used] = '\0';
    lockdock_ipc_trim_trailing_whitespace(buffer);
    if (buffer[0] == '\0') {
        snprintf(error, error_size, "Daemon sent an empty %s", kind);
        return false;
    }

    return true;
}

static bool lockdock_ipc_request_response(const LockDockIpcRequest *request,
                                          LockDockIpcResponse *response_out,
                                          char *error,
                                          size_t error_size) {
    char socket_path[PATH_MAX];
    char request_json[LOCKDOCK_IPC_MAX_MESSAGE];
    char response_json[LOCKDOCK_IPC_MAX_MESSAGE];
    struct sockaddr_un addr;
    int fd = -1;
    bool success = false;

    if (request == NULL || response_out == NULL) {
        lockdock_ipc_set_error(error, error_size, "Internal error");
        return false;
    }

    if (!lockdock_ipc_copy_socket_path(socket_path, sizeof(socket_path), error,
                                       error_size) ||
        !lockdock_ipc_serialize_request_json(
            request, request_json, sizeof(request_json), error, error_size)) {
        return false;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(error, error_size, "Failed to create daemon socket: %s",
                 strerror(errno));
        return false;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_len = sizeof(addr);
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        snprintf(error, error_size, "Failed to connect to daemon socket '%s': %s",
                 socket_path, strerror(errno));
        goto cleanup;
    }

    if (!lockdock_ipc_write_all(fd, request_json, strlen(request_json), error,
                                error_size)) {
        goto cleanup;
    }

    if (shutdown(fd, SHUT_WR) != 0) {
        snprintf(error, error_size, "Failed to finalize daemon request: %s",
                 strerror(errno));
        goto cleanup;
    }

    if (!lockdock_ipc_read_message(fd, response_json, sizeof(response_json),
                                   "response", error, error_size) ||
        !lockdock_ipc_parse_response_json(response_json, response_out, error,
                                          error_size)) {
        goto cleanup;
    }

    success = true;

cleanup:
    if (fd >= 0) {
        close(fd);
    }

    return success;
}

static bool lockdock_ipc_expect_result(const LockDockIpcResponse *response,
                                       char *error,
                                       size_t error_size) {
    if (response == NULL) {
        lockdock_ipc_set_error(error, error_size, "Internal error");
        return false;
    }

    if (response->kind != LOCKDOCK_IPC_RESPONSE_RESULT) {
        lockdock_ipc_set_error(error, error_size,
                               "Daemon returned an unexpected response");
        return false;
    }

    if (!response->result.success) {
        if (response->result.reason[0] != '\0') {
            lockdock_ipc_set_error(error, error_size, response->result.reason);
        } else {
            lockdock_ipc_set_error(error, error_size, "Daemon request failed");
        }
        return false;
    }

    return true;
}

bool lockdock_ipc_ensure_socket_dir(char *error, size_t error_size) {
    char dir_path[PATH_MAX];

    if (!lockdock_ipc_copy_socket_dir(dir_path, sizeof(dir_path), error,
                                      error_size)) {
        return false;
    }

    return lockdock_ipc_mkdir_p(dir_path, 0700, error, error_size);
}

bool lockdock_ipc_copy_socket_path(char *buffer,
                                   size_t buffer_size,
                                   char *error,
                                   size_t error_size) {
    char dir_path[PATH_MAX];

    if (!lockdock_ipc_copy_socket_dir(dir_path, sizeof(dir_path), error,
                                      error_size)) {
        return false;
    }

    if (snprintf(buffer, buffer_size, "%s/control.sock", dir_path) >=
        (int)buffer_size) {
        lockdock_ipc_set_error(error, error_size, "Socket path is too long");
        return false;
    }

    if (strlen(buffer) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        lockdock_ipc_set_error(error, error_size,
                               "Socket path is too long for a Unix domain socket");
        return false;
    }

    return true;
}

bool lockdock_ipc_copy_pid_path(char *buffer,
                                size_t buffer_size,
                                char *error,
                                size_t error_size) {
    char dir_path[PATH_MAX];

    if (!lockdock_ipc_copy_socket_dir(dir_path, sizeof(dir_path), error,
                                      error_size)) {
        return false;
    }

    if (snprintf(buffer, buffer_size, "%s/daemon.pid", dir_path) >=
        (int)buffer_size) {
        lockdock_ipc_set_error(error, error_size, "PID file path is too long");
        return false;
    }

    return true;
}

bool lockdock_ipc_parse_request_json(const char *request_json,
                                     LockDockIpcRequest *request_out,
                                     char *error,
                                     size_t error_size) {
    json_object_t *request_object = NULL;
    json_value_t *request_root = NULL;
    json_object_element_t *element;
    bool success = false;

    if (request_json == NULL || request_out == NULL) {
        lockdock_ipc_set_error(error, error_size, "Internal error");
        return false;
    }

    memset(request_out, 0, sizeof(*request_out));

    if (!lockdock_ipc_parse_json_object(request_json, "request", &request_object,
                                        &request_root, error, error_size)) {
        return false;
    }

    for (element = request_object->start; element != NULL; element = element->next) {
        if (!lockdock_ipc_parse_request_field(element, request_out, error,
                                              error_size)) {
            goto cleanup;
        }
    }

    success = lockdock_ipc_validate_request(request_out, error, error_size);

cleanup:
    free(request_root);
    return success;
}

bool lockdock_ipc_serialize_request_json(const LockDockIpcRequest *request,
                                         char *buffer,
                                         size_t buffer_size,
                                         char *error,
                                         size_t error_size) {
    const char *command_name = NULL;
    size_t used = 0;

    if (request == NULL || buffer == NULL) {
        lockdock_ipc_set_error(error, error_size, "Internal error");
        return false;
    }

    if (!lockdock_ipc_validate_request(request, error, error_size) ||
        !lockdock_ipc_command_to_string(request->command, &command_name)) {
        if (command_name == NULL && error != NULL && error[0] == '\0') {
            lockdock_ipc_set_error(error, error_size, "Unknown command");
        }
        return false;
    }

    buffer[0] = '\0';
    if (!lockdock_ipc_append_bytes(buffer, buffer_size, &used,
                                   "{\"cmd\":", sizeof("{\"cmd\":") - 1) ||
        !lockdock_ipc_append_json_string(buffer, buffer_size, &used, command_name)) {
        lockdock_ipc_set_error(error, error_size, "Request buffer is too small");
        return false;
    }

    if (request->command == LOCKDOCK_IPC_COMMAND_SET_STATE &&
        !lockdock_ipc_append_format(buffer, buffer_size, &used, ",\"target\":%d",
                                    request->target)) {
        lockdock_ipc_set_error(error, error_size, "Request buffer is too small");
        return false;
    }

    if (!lockdock_ipc_append_bytes(buffer, buffer_size, &used, "}", 1)) {
        lockdock_ipc_set_error(error, error_size, "Request buffer is too small");
        return false;
    }

    return true;
}

bool lockdock_ipc_parse_response_json(const char *response_json,
                                      LockDockIpcResponse *response_out,
                                      char *error,
                                      size_t error_size) {
    json_object_t *response_object = NULL;
    json_value_t *response_root = NULL;
    json_object_element_t *element;
    LockDockIpcState parsed_state;
    LockDockIpcResult parsed_result;
    bool has_displays = false;
    bool has_location = false;
    bool has_target = false;
    bool has_success = false;
    bool has_reason = false;
    bool success = false;

    if (response_json == NULL || response_out == NULL) {
        lockdock_ipc_set_error(error, error_size, "Internal error");
        return false;
    }

    memset(response_out, 0, sizeof(*response_out));
    memset(&parsed_state, 0, sizeof(parsed_state));
    memset(&parsed_result, 0, sizeof(parsed_result));

    if (!lockdock_ipc_parse_json_object(response_json, "response", &response_object,
                                        &response_root, error, error_size)) {
        return false;
    }

    if (response_object->length == 0) {
        lockdock_ipc_set_error(error, error_size, "Response cannot be empty");
        goto cleanup;
    }

    for (element = response_object->start; element != NULL;
         element = element->next) {
        if (!lockdock_ipc_parse_response_field(
                element, &parsed_state, &parsed_result, &has_displays, &has_location,
                &has_target, &has_success, &has_reason, error, error_size)) {
            goto cleanup;
        }
    }

    if (!lockdock_ipc_validate_response(has_displays, has_location, has_target,
                                        has_success, has_reason, error,
                                        error_size)) {
        goto cleanup;
    }

    if (has_success) {
        response_out->kind = LOCKDOCK_IPC_RESPONSE_RESULT;
        response_out->result = parsed_result;
        success = true;
        goto cleanup;
    }

    parsed_state.has_target = has_target;
    response_out->kind = LOCKDOCK_IPC_RESPONSE_STATE;
    response_out->state = parsed_state;
    success = true;

cleanup:
    free(response_root);
    return success;
}

bool lockdock_ipc_serialize_state_response_json(const LockDockIpcState *state,
                                                char *buffer,
                                                size_t buffer_size,
                                                char *error,
                                                size_t error_size) {
    size_t used = 0;

    if (state == NULL || buffer == NULL) {
        lockdock_ipc_set_error(error, error_size, "Internal error");
        return false;
    }

    if (state->display_count > LOCKDOCK_IPC_MAX_DISPLAYS ||
        state->location_index < 0 ||
        (state->has_target && state->target_index < 0)) {
        lockdock_ipc_set_error(error, error_size, "Invalid state response");
        return false;
    }

    buffer[0] = '\0';
    if (!lockdock_ipc_append_bytes(buffer, buffer_size, &used, "{\"displays\":[",
                                   sizeof("{\"displays\":[") - 1)) {
        lockdock_ipc_set_error(error, error_size,
                               "State response buffer is too small");
        return false;
    }

    for (uint32_t i = 0; i < state->display_count; i++) {
        if (i > 0 &&
            !lockdock_ipc_append_bytes(buffer, buffer_size, &used, ",", 1)) {
            lockdock_ipc_set_error(error, error_size,
                                   "State response buffer is too small");
            return false;
        }

        if (!lockdock_ipc_append_json_string(buffer, buffer_size, &used,
                                             state->displays[i])) {
            lockdock_ipc_set_error(error, error_size,
                                   "State response buffer is too small");
            return false;
        }
    }

    if (!lockdock_ipc_append_format(buffer, buffer_size, &used, "],\"location\":%d",
                                    state->location_index)) {
        lockdock_ipc_set_error(error, error_size,
                               "State response buffer is too small");
        return false;
    }

    if (state->has_target &&
        !lockdock_ipc_append_format(buffer, buffer_size, &used, ",\"target\":%d",
                                    state->target_index)) {
        lockdock_ipc_set_error(error, error_size,
                               "State response buffer is too small");
        return false;
    }

    if (!lockdock_ipc_append_bytes(buffer, buffer_size, &used, "}", 1)) {
        lockdock_ipc_set_error(error, error_size,
                               "State response buffer is too small");
        return false;
    }

    return true;
}

bool lockdock_ipc_serialize_result_response_json(const LockDockIpcResult *result,
                                                 char *buffer,
                                                 size_t buffer_size,
                                                 char *error,
                                                 size_t error_size) {
    size_t used = 0;

    if (result == NULL || buffer == NULL) {
        lockdock_ipc_set_error(error, error_size, "Internal error");
        return false;
    }

    buffer[0] = '\0';
    if (!lockdock_ipc_append_format(buffer, buffer_size, &used, "{\"success\":%s}",
                                    result->success ? "true" : "false")) {
        lockdock_ipc_set_error(error, error_size,
                               "Result response buffer is too small");
        return false;
    }

    if (!result->success) {
        used = 0;
        buffer[0] = '\0';

        if (!lockdock_ipc_append_bytes(
                buffer, buffer_size, &used, "{\"success\":false,\"reason\":",
                sizeof("{\"success\":false,\"reason\":") - 1) ||
            !lockdock_ipc_append_json_string(buffer, buffer_size, &used,
                                             result->reason) ||
            !lockdock_ipc_append_bytes(buffer, buffer_size, &used, "}", 1)) {
            lockdock_ipc_set_error(error, error_size,
                                   "Result response buffer is too small");
            return false;
        }
    }

    return true;
}

bool lockdock_ipc_get_state(LockDockIpcState *state_out,
                            char *error,
                            size_t error_size) {
    LockDockIpcRequest request;
    LockDockIpcResponse response;

    if (state_out == NULL) {
        lockdock_ipc_set_error(error, error_size, "Internal error");
        return false;
    }

    memset(&request, 0, sizeof(request));
    request.command = LOCKDOCK_IPC_COMMAND_GET_STATE;

    if (!lockdock_ipc_request_response(&request, &response, error, error_size)) {
        return false;
    }

    if (response.kind == LOCKDOCK_IPC_RESPONSE_RESULT) {
        if (!lockdock_ipc_expect_result(&response, error, error_size)) {
            return false;
        }

        lockdock_ipc_set_error(error, error_size,
                               "Daemon returned an unexpected response");
        return false;
    }

    if (response.kind != LOCKDOCK_IPC_RESPONSE_STATE) {
        lockdock_ipc_set_error(error, error_size,
                               "Daemon returned an unexpected response");
        return false;
    }

    *state_out = response.state;
    return true;
}

bool lockdock_ipc_lock(int target_index, char *error, size_t error_size) {
    LockDockIpcRequest request;
    LockDockIpcResponse response;

    if (target_index < 0) {
        lockdock_ipc_set_error(error, error_size,
                               "Display index must be a non-negative integer");
        return false;
    }

    memset(&request, 0, sizeof(request));
    request.command = LOCKDOCK_IPC_COMMAND_SET_STATE;
    request.has_target = true;
    request.target = target_index;

    if (!lockdock_ipc_request_response(&request, &response, error, error_size)) {
        return false;
    }

    return lockdock_ipc_expect_result(&response, error, error_size);
}

bool lockdock_ipc_unlock(char *error, size_t error_size) {
    LockDockIpcRequest request;
    LockDockIpcResponse response;

    memset(&request, 0, sizeof(request));
    request.command = LOCKDOCK_IPC_COMMAND_UNLOCK;

    if (!lockdock_ipc_request_response(&request, &response, error, error_size)) {
        return false;
    }

    return lockdock_ipc_expect_result(&response, error, error_size);
}
