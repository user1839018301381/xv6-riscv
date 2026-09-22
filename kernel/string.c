#include "types.h"

void *memset(void *destination, int value, uint byte_count)
{
    char *destination_bytes = (char *)destination;
    int i;
    for (i = 0; i < byte_count; i++) {
        destination_bytes[i] = value;
    }
    return destination;
}

int memcmp(const void *left, const void *right, uint byte_count)
{
    const uchar *left_bytes, *right_bytes;

    left_bytes = left;
    right_bytes = right;
    while (byte_count-- > 0) {
        if (*left_bytes != *right_bytes)
            return *left_bytes - *right_bytes;
        left_bytes++, right_bytes++;
    }

    return 0;
}

void *memmove(void *destination, const void *source, uint byte_count)
{
    const char *source_bytes;
    char *destination_bytes;

    if (byte_count == 0)
        return destination;

    source_bytes = source;
    destination_bytes = destination;
    if (source_bytes < destination_bytes &&
        source_bytes + byte_count > destination_bytes) {
        source_bytes += byte_count;
        destination_bytes += byte_count;
        while (byte_count-- > 0)
            *--destination_bytes = *--source_bytes;
    } else
        while (byte_count-- > 0)
            *destination_bytes++ = *source_bytes++;

    return destination;
}

// memcpyはGCCを納得させるために存在する。memmoveを使う。
void *memcpy(void *destination, const void *source, uint byte_count)
{
    return memmove(destination, source, byte_count);
}

int strncmp(const char *left, const char *right, uint max_length)
{
    while (max_length > 0 && *left && *left == *right)
        max_length--, left++, right++;
    if (max_length == 0)
        return 0;
    return (uchar)*left - (uchar)*right;
}

char *strncpy(char *destination, const char *source, int max_length)
{
    char *original_destination;

    original_destination = destination;
    while (max_length-- > 0 && (*destination++ = *source++) != 0)
        ;
    while (max_length-- > 0)
        *destination++ = 0;
    return original_destination;
}

// strncpyに似ているが、必ずNUL終端する。
char *safestrcpy(char *destination, const char *source, int destination_size)
{
    char *original_destination;

    original_destination = destination;
    if (destination_size <= 0)
        return original_destination;
    while (--destination_size > 0 && (*destination++ = *source++) != 0)
        ;
    *destination = 0;
    return original_destination;
}

int strlen(const char *string)
{
    int length;

    for (length = 0; string[length]; length++)
        ;
    return length;
}
