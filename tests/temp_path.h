#ifndef PORTHOLE_TEST_TEMP_PATH_H
#define PORTHOLE_TEST_TEMP_PATH_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#endif

static inline int ph_test_temp_file(char *path, size_t capacity)
{
#ifdef _WIN32
    char directory[MAX_PATH];
    DWORD length = GetTempPathA(sizeof directory, directory);
    if (!length || length >= sizeof directory || capacity < MAX_PATH ||
        !GetTempFileNameA(directory, "pht", 0, path))
        return -1;
    int fd = _open(path, _O_RDWR | _O_BINARY);
    if (fd < 0)
        remove(path);
    return fd;
#else
    int length = snprintf(path, capacity, "/tmp/porthole-test-XXXXXX");
    if (length < 0 || (size_t)length >= capacity)
        return -1;
    return mkstemp(path);
#endif
}

static inline bool ph_test_temp_dir(char *path, size_t capacity)
{
#ifdef _WIN32
    int fd = ph_test_temp_file(path, capacity);
    if (fd < 0)
        return false;
    close(fd);
    return remove(path) == 0 && _mkdir(path) == 0;
#else
    int length = snprintf(path, capacity, "/tmp/porthole-test-XXXXXX");
    if (length < 0 || (size_t)length >= capacity)
        return false;
    return mkdtemp(path) != NULL;
#endif
}

#endif
