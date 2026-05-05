#include "lockdockd_request.h"

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOCKDOCKD_REQUEST_FIELD_NAME_SIZE 64
#define LOCKDOCKD_REQUEST_TOKEN_TEXT_SIZE 512

typedef enum {
    LOCKDOCKD_REQUEST_TOKEN_EOF = 0,
    LOCKDOCKD_REQUEST_TOKEN_START_OBJECT,
    LOCKDOCKD_REQUEST_TOKEN_END_OBJECT,
    LOCKDOCKD_REQUEST_TOKEN_COLON,
    LOCKDOCKD_REQUEST_TOKEN_COMMA,
    LOCKDOCKD_REQUEST_TOKEN_STRING,
    LOCKDOCKD_REQUEST_TOKEN_INT64,
    LOCKDOCKD_REQUEST_TOKEN_UINT64
} LockDockdRequestTokenType;

typedef struct {
    LockDockdRequestTokenType type;
    char text[LOCKDOCKD_REQUEST_TOKEN_TEXT_SIZE];
} LockDockdRequestToken;

static void lockdockd_request_set_error(char *buffer,
                                        size_t buffer_size,
                                        const char *message) {
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    snprintf(buffer, buffer_size, "%s", message);
}

static const char *lockdockd_request_skip_whitespace(const char *cursor) {
    while (cursor != NULL && *cursor != '\0' &&
           isspace((unsigned char)*cursor) != 0) {
        cursor++;
    }

    return cursor;
}

static bool lockdockd_request_append_char(char *buffer,
                                          size_t buffer_size,
                                          size_t *used,
                                          char value,
                                          char *error,
                                          size_t error_size) {
    if (*used + 1 >= buffer_size) {
        lockdockd_request_set_error(error, error_size, "Request string is too long");
        return false;
    }

    buffer[*used] = value;
    (*used)++;
    buffer[*used] = '\0';
    return true;
}

static bool lockdockd_request_parse_hex4(const char *cursor, uint32_t *value_out) {
    uint32_t value = 0;

    if (cursor == NULL || value_out == NULL) {
        return false;
    }

    for (int i = 0; i < 4; i++) {
        unsigned char ch = (unsigned char)cursor[i];
        uint32_t digit;

        if (ch >= '0' && ch <= '9') {
            digit = (uint32_t)(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            digit = (uint32_t)(ch - 'a' + 10);
        } else if (ch >= 'A' && ch <= 'F') {
            digit = (uint32_t)(ch - 'A' + 10);
        } else {
            return false;
        }

        value = (value << 4) | digit;
    }

    *value_out = value;
    return true;
}

static bool lockdockd_request_has_chars(const char *cursor, size_t count) {
    if (cursor == NULL) {
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        if (cursor[i] == '\0') {
            return false;
        }
    }

    return true;
}

static bool lockdockd_request_append_utf8(char *buffer,
                                          size_t buffer_size,
                                          size_t *used,
                                          uint32_t codepoint,
                                          char *error,
                                          size_t error_size) {
    if (codepoint <= 0x7F) {
        return lockdockd_request_append_char(buffer, buffer_size, used,
                                             (char)codepoint, error, error_size);
    }

    if (codepoint <= 0x7FF) {
        return lockdockd_request_append_char(buffer, buffer_size, used,
                                             (char)(0xC0 | (codepoint >> 6)), error,
                                             error_size) &&
               lockdockd_request_append_char(buffer, buffer_size, used,
                                             (char)(0x80 | (codepoint & 0x3F)),
                                             error, error_size);
    }

    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
        lockdockd_request_set_error(error, error_size,
                                    "Invalid JSON request: unsupported unicode "
                                    "surrogate pair");
        return false;
    }

    return lockdockd_request_append_char(buffer, buffer_size, used,
                                         (char)(0xE0 | (codepoint >> 12)), error,
                                         error_size) &&
           lockdockd_request_append_char(buffer, buffer_size, used,
                                         (char)(0x80 | ((codepoint >> 6) & 0x3F)),
                                         error, error_size) &&
           lockdockd_request_append_char(buffer, buffer_size, used,
                                         (char)(0x80 | (codepoint & 0x3F)), error,
                                         error_size);
}

static bool lockdockd_request_parse_string_token(const char **cursor,
                                                 LockDockdRequestToken *token,
                                                 char *error,
                                                 size_t error_size) {
    const char *current = *cursor + 1;
    size_t used = 0;

    token->type = LOCKDOCKD_REQUEST_TOKEN_STRING;
    token->text[0] = '\0';

    while (*current != '\0') {
        unsigned char ch = (unsigned char)*current++;

        if (ch == '"') {
            *cursor = current;
            return true;
        }

        if (ch < 0x20) {
            lockdockd_request_set_error(error, error_size,
                                        "Invalid JSON request: control characters "
                                        "are not allowed in strings");
            return false;
        }

        if (ch != '\\') {
            if (!lockdockd_request_append_char(token->text, sizeof(token->text),
                                               &used, (char)ch, error, error_size)) {
                return false;
            }
            continue;
        }

        ch = (unsigned char)*current++;
        if (ch == '\0') {
            lockdockd_request_set_error(
                error, error_size,
                "Invalid JSON request: unterminated escape sequence");
            return false;
        }

        switch (ch) {
            case '"':
            case '\\':
            case '/':
                if (!lockdockd_request_append_char(token->text, sizeof(token->text),
                                                   &used, (char)ch, error,
                                                   error_size)) {
                    return false;
                }
                break;

            case 'b':
                if (!lockdockd_request_append_char(token->text, sizeof(token->text),
                                                   &used, '\b', error, error_size)) {
                    return false;
                }
                break;

            case 'f':
                if (!lockdockd_request_append_char(token->text, sizeof(token->text),
                                                   &used, '\f', error, error_size)) {
                    return false;
                }
                break;

            case 'n':
                if (!lockdockd_request_append_char(token->text, sizeof(token->text),
                                                   &used, '\n', error, error_size)) {
                    return false;
                }
                break;

            case 'r':
                if (!lockdockd_request_append_char(token->text, sizeof(token->text),
                                                   &used, '\r', error, error_size)) {
                    return false;
                }
                break;

            case 't':
                if (!lockdockd_request_append_char(token->text, sizeof(token->text),
                                                   &used, '\t', error, error_size)) {
                    return false;
                }
                break;

            case 'u': {
                uint32_t codepoint = 0;

                if (!lockdockd_request_has_chars(current, 4)) {
                    lockdockd_request_set_error(
                        error, error_size,
                        "Invalid JSON request: malformed unicode escape");
                    return false;
                }

                if (!lockdockd_request_parse_hex4(current, &codepoint)) {
                    lockdockd_request_set_error(
                        error, error_size,
                        "Invalid JSON request: malformed unicode escape");
                    return false;
                }

                current += 4;
                if (!lockdockd_request_append_utf8(token->text, sizeof(token->text),
                                                   &used, codepoint, error,
                                                   error_size)) {
                    return false;
                }
                break;
            }

            default:
                lockdockd_request_set_error(
                    error, error_size,
                    "Invalid JSON request: unsupported escape sequence");
                return false;
        }
    }

    lockdockd_request_set_error(error, error_size,
                                "Invalid JSON request: unterminated string");
    return false;
}

static bool lockdockd_request_parse_number_token(const char **cursor,
                                                 LockDockdRequestToken *token,
                                                 char *error,
                                                 size_t error_size) {
    const char *start = *cursor;
    const char *current = start;
    bool negative = false;
    size_t length;

    if (*current == '-') {
        negative = true;
        current++;
    }

    if (!isdigit((unsigned char)*current)) {
        lockdockd_request_set_error(error, error_size,
                                    "Invalid JSON request: malformed number");
        return false;
    }

    if (*current == '0' && isdigit((unsigned char)current[1]) != 0) {
        lockdockd_request_set_error(error, error_size,
                                    "Invalid JSON request: malformed number");
        return false;
    }

    while (isdigit((unsigned char)*current) != 0) {
        current++;
    }

    if (*current == '.' || *current == 'e' || *current == 'E') {
        lockdockd_request_set_error(error, error_size,
                                    "Request must be a single flat JSON object");
        return false;
    }

    length = (size_t)(current - start);
    if (length >= sizeof(token->text)) {
        lockdockd_request_set_error(error, error_size, "Request value is too long");
        return false;
    }

    memcpy(token->text, start, length);
    token->text[length] = '\0';
    token->type =
        negative ? LOCKDOCKD_REQUEST_TOKEN_INT64 : LOCKDOCKD_REQUEST_TOKEN_UINT64;
    *cursor = current;
    return true;
}

static bool lockdockd_request_next_token(const char **cursor,
                                         LockDockdRequestToken *token,
                                         char *error,
                                         size_t error_size) {
    const char *current = lockdockd_request_skip_whitespace(*cursor);

    if (current == NULL || token == NULL) {
        lockdockd_request_set_error(error, error_size, "Internal error");
        return false;
    }

    token->text[0] = '\0';

    switch (*current) {
        case '\0':
            token->type = LOCKDOCKD_REQUEST_TOKEN_EOF;
            *cursor = current;
            return true;

        case '{':
            token->type = LOCKDOCKD_REQUEST_TOKEN_START_OBJECT;
            *cursor = current + 1;
            return true;

        case '}':
            token->type = LOCKDOCKD_REQUEST_TOKEN_END_OBJECT;
            *cursor = current + 1;
            return true;

        case ':':
            token->type = LOCKDOCKD_REQUEST_TOKEN_COLON;
            *cursor = current + 1;
            return true;

        case ',':
            token->type = LOCKDOCKD_REQUEST_TOKEN_COMMA;
            *cursor = current + 1;
            return true;

        case '"':
            *cursor = current;
            return lockdockd_request_parse_string_token(cursor, token, error,
                                                        error_size);

        default:
            if (*current == '-' || isdigit((unsigned char)*current) != 0) {
                *cursor = current;
                return lockdockd_request_parse_number_token(cursor, token, error,
                                                            error_size);
            }

            lockdockd_request_set_error(error, error_size,
                                        "Request must be a single flat JSON object");
            return false;
    }
}

static bool lockdockd_request_convert_command(const char *value,
                                              LockDockdRequest *request,
                                              char *error,
                                              size_t error_size) {
    if (request->command != LOCKDOCKD_REQUEST_NONE) {
        lockdockd_request_set_error(error, error_size,
                                    "Field 'cmd' must be specified once");
        return false;
    }

    if (value == NULL) {
        lockdockd_request_set_error(error, error_size, "Internal error");
        return false;
    }

    if (strcmp(value, "get_state") == 0) {
        request->command = LOCKDOCKD_REQUEST_GET_STATE;
        return true;
    }

    if (strcmp(value, "set_state") == 0) {
        request->command = LOCKDOCKD_REQUEST_SET_STATE;
        return true;
    }

    if (strcmp(value, "unlock") == 0) {
        request->command = LOCKDOCKD_REQUEST_UNLOCK;
        return true;
    }

    lockdockd_request_set_error(error, error_size, "Unknown command");
    return false;
}

static bool lockdockd_request_convert_target(LockDockdRequestTokenType token_type,
                                             const char *value,
                                             int *target_out,
                                             char *error,
                                             size_t error_size) {
    char *endptr = NULL;

    if (value == NULL || target_out == NULL) {
        lockdockd_request_set_error(error, error_size, "Internal error");
        return false;
    }

    if (token_type == LOCKDOCKD_REQUEST_TOKEN_INT64) {
        long long parsed = strtoll(value, &endptr, 10);

        if (endptr == value || *endptr != '\0') {
            lockdockd_request_set_error(error, error_size,
                                        "Field 'target' must be an integer");
            return false;
        }

        if (parsed < 0 || parsed > INT_MAX) {
            lockdockd_request_set_error(
                error, error_size,
                "Field 'target' must be a non-negative display index");
            return false;
        }

        *target_out = (int)parsed;
        return true;
    }

    if (token_type == LOCKDOCKD_REQUEST_TOKEN_UINT64) {
        unsigned long long parsed = strtoull(value, &endptr, 10);

        if (endptr == value || *endptr != '\0') {
            lockdockd_request_set_error(error, error_size,
                                        "Field 'target' must be an integer");
            return false;
        }

        if (parsed > INT_MAX) {
            lockdockd_request_set_error(
                error, error_size,
                "Field 'target' must be a non-negative display index");
            return false;
        }

        *target_out = (int)parsed;
        return true;
    }

    lockdockd_request_set_error(error, error_size,
                                "Field 'target' must be an integer");
    return false;
}

static bool lockdockd_request_parse_field(const char *name,
                                          const LockDockdRequestToken *value_token,
                                          LockDockdRequest *request,
                                          char *error,
                                          size_t error_size) {
    if (name == NULL || value_token == NULL || request == NULL) {
        lockdockd_request_set_error(error, error_size, "Internal error");
        return false;
    }

    if (strcmp(name, "cmd") == 0) {
        if (value_token->type != LOCKDOCKD_REQUEST_TOKEN_STRING) {
            lockdockd_request_set_error(error, error_size,
                                        "Field 'cmd' must be a string");
            return false;
        }

        return lockdockd_request_convert_command(value_token->text, request, error,
                                                 error_size);
    }

    if (strcmp(name, "target") == 0) {
        if (request->has_target) {
            lockdockd_request_set_error(error, error_size,
                                        "Field 'target' must be specified once");
            return false;
        }

        if (!lockdockd_request_convert_target(value_token->type, value_token->text,
                                              &request->target, error, error_size)) {
            return false;
        }

        request->has_target = true;
        return true;
    }

    snprintf(error, error_size, "Unknown field '%s'", name);
    return false;
}

static bool lockdockd_request_validate(const LockDockdRequest *request,
                                       char *error,
                                       size_t error_size) {
    if (request == NULL) {
        lockdockd_request_set_error(error, error_size, "Internal error");
        return false;
    }

    if (request->command == LOCKDOCKD_REQUEST_NONE) {
        lockdockd_request_set_error(error, error_size,
                                    "Missing required field 'cmd'");
        return false;
    }

    if (request->command == LOCKDOCKD_REQUEST_SET_STATE) {
        if (!request->has_target) {
            lockdockd_request_set_error(error, error_size,
                                        "Missing required field 'target'");
            return false;
        }
    } else if (request->has_target) {
        lockdockd_request_set_error(
            error, error_size,
            "Field 'target' is only valid for command 'set_state'");
        return false;
    }

    return true;
}

bool lockdockd_parse_request_json(const char *request_json,
                                  LockDockdRequest *request_out,
                                  char *error,
                                  size_t error_size) {
    const char *cursor = request_json;
    LockDockdRequestToken token;

    if (request_json == NULL || request_out == NULL) {
        lockdockd_request_set_error(error, error_size, "Internal error");
        return false;
    }

    memset(request_out, 0, sizeof(*request_out));

    if (!lockdockd_request_next_token(&cursor, &token, error, error_size)) {
        return false;
    }

    if (token.type != LOCKDOCKD_REQUEST_TOKEN_START_OBJECT) {
        lockdockd_request_set_error(error, error_size,
                                    "Request must be a single JSON object");
        return false;
    }

    if (!lockdockd_request_next_token(&cursor, &token, error, error_size)) {
        return false;
    }

    if (token.type == LOCKDOCKD_REQUEST_TOKEN_END_OBJECT) {
        return lockdockd_request_validate(request_out, error, error_size);
    }

    while (true) {
        char field_name[LOCKDOCKD_REQUEST_FIELD_NAME_SIZE];

        if (token.type != LOCKDOCKD_REQUEST_TOKEN_STRING) {
            lockdockd_request_set_error(error, error_size,
                                        "Request must be a single flat JSON object");
            return false;
        }

        if (snprintf(field_name, sizeof(field_name), "%s", token.text) >=
            (int)sizeof(field_name)) {
            lockdockd_request_set_error(error, error_size,
                                        "Request field name is too long");
            return false;
        }

        if (!lockdockd_request_next_token(&cursor, &token, error, error_size)) {
            return false;
        }

        if (token.type != LOCKDOCKD_REQUEST_TOKEN_COLON) {
            lockdockd_request_set_error(error, error_size,
                                        "Request must be a single flat JSON object");
            return false;
        }

        if (!lockdockd_request_next_token(&cursor, &token, error, error_size)) {
            return false;
        }

        if (token.type != LOCKDOCKD_REQUEST_TOKEN_STRING &&
            token.type != LOCKDOCKD_REQUEST_TOKEN_INT64 &&
            token.type != LOCKDOCKD_REQUEST_TOKEN_UINT64) {
            lockdockd_request_set_error(error, error_size,
                                        "Request must be a single flat JSON object");
            return false;
        }

        if (!lockdockd_request_parse_field(field_name, &token, request_out, error,
                                           error_size)) {
            return false;
        }

        if (!lockdockd_request_next_token(&cursor, &token, error, error_size)) {
            return false;
        }

        if (token.type == LOCKDOCKD_REQUEST_TOKEN_END_OBJECT) {
            break;
        }

        if (token.type != LOCKDOCKD_REQUEST_TOKEN_COMMA) {
            lockdockd_request_set_error(error, error_size,
                                        "Request must be a single flat JSON object");
            return false;
        }

        if (!lockdockd_request_next_token(&cursor, &token, error, error_size)) {
            return false;
        }
    }

    if (!lockdockd_request_next_token(&cursor, &token, error, error_size)) {
        return false;
    }

    if (token.type != LOCKDOCKD_REQUEST_TOKEN_EOF) {
        lockdockd_request_set_error(error, error_size,
                                    "Request must be a single JSON object");
        return false;
    }

    return lockdockd_request_validate(request_out, error, error_size);
}
