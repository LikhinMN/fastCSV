#include "mmap_io.h"
#include <stdio.h>
#include <stddef.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#endif

int mmap_open(const char *path, MmapFile *out) {
    if (!out) return -1;
    out->data = NULL;
    out->len = 0;
    out->handle = NULL;

#ifdef _WIN32
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        perror("mmap_open CreateFileA failed");
        return -1;
    }

    LARGE_INTEGER li;
    if (!GetFileSizeEx(hFile, &li)) {
        perror("mmap_open GetFileSizeEx failed");
        CloseHandle(hFile);
        return -1;
    }

    if (li.QuadPart == 0) {
        CloseHandle(hFile);
        return 0; /* Empty file */
    }

    HANDLE hMapping = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (hMapping == NULL) {
        perror("mmap_open CreateFileMappingA failed");
        CloseHandle(hFile);
        return -1;
    }

    void *data = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
    if (data == NULL) {
        perror("mmap_open MapViewOfFile failed");
        CloseHandle(hMapping);
        CloseHandle(hFile);
        return -1;
    }

    CloseHandle(hFile);

    out->data = (const char *)data;
    out->len = (size_t)li.QuadPart;
    out->handle = hMapping;

    return 0;
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("mmap_open open failed");
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("mmap_open fstat failed");
        close(fd);
        return -1;
    }

    if (st.st_size == 0) {
        close(fd);
        return 0; /* Empty file */
    }

    int flags = MAP_PRIVATE;
#ifdef MAP_POPULATE
    flags |= MAP_POPULATE;
#endif

    void *data = mmap(NULL, st.st_size, PROT_READ, flags, fd, 0);
    if (data == MAP_FAILED) {
        perror("mmap_open mmap failed");
        close(fd);
        return -1;
    }

    close(fd);

    out->data = (const char *)data;
    out->len = st.st_size;
    out->handle = NULL; 

    return 0;
#endif
}

void mmap_close(MmapFile *mf) {
    if (!mf) return;

    if (mf->data && mf->len > 0) {
#ifdef _WIN32
        UnmapViewOfFile(mf->data);
        if (mf->handle) {
            CloseHandle((HANDLE)mf->handle);
        }
#else
        munmap((void *)mf->data, mf->len);
#endif
    }

    mf->data = NULL;
    mf->len = 0;
    mf->handle = NULL;
}
