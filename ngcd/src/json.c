#include "ngcd.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_space(const char *cursor, const char *end)
{
    while (cursor < end && isspace((unsigned char)*cursor))
        ++cursor;
    return cursor;
}

static int hex_digit(unsigned char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

static int append_utf8(char *output, size_t size, size_t *used,
                       unsigned int codepoint)
{
    unsigned char bytes[4];
    size_t count;
    size_t index;

    if (codepoint <= 0x7fU) {
        bytes[0] = (unsigned char)codepoint;
        count = 1;
    } else if (codepoint <= 0x7ffU) {
        bytes[0] = (unsigned char)(0xc0U | (codepoint >> 6));
        bytes[1] = (unsigned char)(0x80U | (codepoint & 0x3fU));
        count = 2;
    } else if (codepoint >= 0xd800U && codepoint <= 0xdfffU) {
        return -1;
    } else {
        bytes[0] = (unsigned char)(0xe0U | (codepoint >> 12));
        bytes[1] = (unsigned char)(0x80U | ((codepoint >> 6) & 0x3fU));
        bytes[2] = (unsigned char)(0x80U | (codepoint & 0x3fU));
        count = 3;
    }
    if (*used + count >= size)
        return -1;
    for (index = 0; index < count; ++index)
        output[(*used)++] = (char)bytes[index];
    return 0;
}

static int parse_string(const char **cursor_pointer, const char *end,
                        char *output, size_t output_size)
{
    const char *cursor = *cursor_pointer;
    size_t used = 0;

    if (cursor >= end || *cursor != '"' || output_size == 0)
        return -1;
    ++cursor;
    while (cursor < end && *cursor != '"') {
        unsigned char byte = (unsigned char)*cursor++;
        if (byte < 0x20U)
            return -1;
        if (byte != '\\') {
            if (used + 1 >= output_size)
                return -1;
            output[used++] = (char)byte;
            continue;
        }
        if (cursor >= end)
            return -1;
        byte = (unsigned char)*cursor++;
        switch (byte) {
        case '"':
        case '\\':
        case '/':
            if (used + 1 >= output_size)
                return -1;
            output[used++] = (char)byte;
            break;
        case 'b':
        case 'f':
        case 'n':
        case 'r':
        case 't': {
            static const char escaped[] = {'\b', '\f', '\n', '\r', '\t'};
            static const char names[] = {'b', 'f', 'n', 'r', 't'};
            size_t index;
            for (index = 0; index < sizeof(names); ++index)
                if ((unsigned char)names[index] == byte)
                    break;
            if (index == sizeof(names) || used + 1 >= output_size)
                return -1;
            output[used++] = escaped[index];
            break;
        }
        case 'u': {
            unsigned int codepoint = 0;
            int digit;
            int index;
            if (end - cursor < 4)
                return -1;
            for (index = 0; index < 4; ++index) {
                digit = hex_digit((unsigned char)*cursor++);
                if (digit < 0)
                    return -1;
                codepoint = codepoint * 16U + (unsigned int)digit;
            }
            if (append_utf8(output, output_size, &used, codepoint) != 0)
                return -1;
            break;
        }
        default:
            return -1;
        }
    }
    if (cursor >= end || *cursor != '"')
        return -1;
    output[used] = '\0';
    *cursor_pointer = cursor + 1;
    return 0;
}

static int skip_value(const char **cursor_pointer, const char *end, int depth)
{
    const char *cursor = skip_space(*cursor_pointer, end);

    if (depth > 16 || cursor >= end)
        return -1;
    if (*cursor == '"') {
        ++cursor;
        while (cursor < end) {
            if ((unsigned char)*cursor < 0x20U)
                return -1;
            if (*cursor == '\\') {
                ++cursor;
                if (cursor >= end)
                    return -1;
                if (*cursor == 'u') {
                    int index;
                    for (index = 0; index < 4; ++index) {
                        ++cursor;
                        if (cursor >= end ||
                            hex_digit((unsigned char)*cursor) < 0)
                            return -1;
                    }
                } else if (strchr("\"\\/bfnrt", *cursor) == NULL) {
                    return -1;
                }
                ++cursor;
                continue;
            }
            if (*cursor++ == '"') {
                *cursor_pointer = cursor;
                return 0;
            }
        }
        return -1;
    } else if (*cursor == '{' || *cursor == '[') {
        char close = *cursor == '{' ? '}' : ']';
        ++cursor;
        for (;;) {
            cursor = skip_space(cursor, end);
            if (cursor >= end)
                return -1;
            if (*cursor == close) {
                ++cursor;
                break;
            }
            if (close == '}') {
                if (*cursor != '"')
                    return -1;
                if (skip_value(&cursor, end, depth + 1) != 0)
                    return -1;
                cursor = skip_space(cursor, end);
                if (cursor >= end || *cursor++ != ':')
                    return -1;
            }
            if (skip_value(&cursor, end, depth + 1) != 0)
                return -1;
            cursor = skip_space(cursor, end);
            if (cursor < end && *cursor == ',') {
                ++cursor;
                continue;
            }
            if (cursor < end && *cursor == close) {
                ++cursor;
                break;
            }
            return -1;
        }
    } else {
        while (cursor < end && *cursor != ',' && *cursor != '}' &&
               *cursor != ']' && !isspace((unsigned char)*cursor))
            ++cursor;
    }
    *cursor_pointer = cursor;
    return 0;
}

static int find_value(const char *json, size_t length, const char *key,
                      const char **value, const char **end_pointer)
{
    const char *cursor = json;
    const char *end = json + length;
    char name[128];

    cursor = skip_space(cursor, end);
    if (cursor >= end || *cursor++ != '{')
        return -1;
    for (;;) {
        cursor = skip_space(cursor, end);
        if (cursor >= end)
            return -1;
        if (*cursor == '}')
            return 0;
        if (parse_string(&cursor, end, name, sizeof(name)) != 0)
            return -1;
        cursor = skip_space(cursor, end);
        if (cursor >= end || *cursor++ != ':')
            return -1;
        cursor = skip_space(cursor, end);
        if (strcmp(name, key) == 0) {
            *value = cursor;
            *end_pointer = end;
            return 1;
        }
        if (skip_value(&cursor, end, 0) != 0)
            return -1;
        cursor = skip_space(cursor, end);
        if (cursor < end && *cursor == ',') {
            ++cursor;
            continue;
        }
        if (cursor < end && *cursor == '}')
            return 0;
        return -1;
    }
}

int ngcd_json_get_string(const char *json, size_t length, const char *key,
                         char *output, size_t output_size)
{
    const char *value = NULL;
    const char *end = NULL;
    int found = find_value(json, length, key, &value, &end);

    if (found <= 0)
        return found;
    if (*value == '"')
        return parse_string(&value, end, output, output_size) == 0 ? 1 : -1;
    {
        const char *finish = value;
        size_t count;
        while (finish < end && *finish != ',' && *finish != '}' &&
               !isspace((unsigned char)*finish))
            ++finish;
        count = (size_t)(finish - value);
        if (count == 0 || count >= output_size)
            return -1;
        memcpy(output, value, count);
        output[count] = '\0';
    }
    return 1;
}

int ngcd_json_get_int64(const char *json, size_t length, const char *key,
                        int64_t *result)
{
    char text[64];
    char *end = NULL;
    long long value;
    int found = ngcd_json_get_string(json, length, key, text, sizeof(text));

    if (found <= 0)
        return found;
    errno = 0;
    value = strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return -1;
    *result = (int64_t)value;
    return 1;
}

int ngcd_json_get_bool(const char *json, size_t length, const char *key,
                       bool *value)
{
    char text[16];
    int found = ngcd_json_get_string(json, length, key, text, sizeof(text));

    if (found <= 0)
        return found;
    if (strcmp(text, "true") == 0 || strcmp(text, "1") == 0) {
        *value = true;
        return 1;
    }
    if (strcmp(text, "false") == 0 || strcmp(text, "0") == 0) {
        *value = false;
        return 1;
    }
    return -1;
}

int ngcd_json_escape(char *output, size_t output_size, const char *value)
{
    static const char hex[] = "0123456789abcdef";
    size_t used = 0;

    while (*value != '\0') {
        unsigned char byte = (unsigned char)*value++;
        const char *escape = NULL;
        if (byte == '"')
            escape = "\\\"";
        else if (byte == '\\')
            escape = "\\\\";
        else if (byte == '\b')
            escape = "\\b";
        else if (byte == '\f')
            escape = "\\f";
        else if (byte == '\n')
            escape = "\\n";
        else if (byte == '\r')
            escape = "\\r";
        else if (byte == '\t')
            escape = "\\t";
        if (escape != NULL) {
            if (used + 2 >= output_size)
                return -1;
            output[used++] = escape[0];
            output[used++] = escape[1];
        } else if (byte < 0x20U) {
            if (used + 6 >= output_size)
                return -1;
            output[used++] = '\\';
            output[used++] = 'u';
            output[used++] = '0';
            output[used++] = '0';
            output[used++] = hex[byte >> 4];
            output[used++] = hex[byte & 15U];
        } else {
            if (used + 1 >= output_size)
                return -1;
            output[used++] = (char)byte;
        }
    }
    if (used >= output_size)
        return -1;
    output[used] = '\0';
    return (int)used;
}
