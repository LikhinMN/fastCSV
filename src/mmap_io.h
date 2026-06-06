#ifndef FASTCSV_MMAP_IO_H
#define FASTCSV_MMAP_IO_H

#include <stddef.h>

typedef struct {
    const char *data;   /* pointer to mapped memory   */
    size_t      len;    /* file size in bytes          */
    void       *handle; /* platform-specific, internal */
} MmapFile;

/* Open a file and mmap it. Returns 0 on success, -1 on error. */
int  mmap_open(const char *path, MmapFile *out);

/* Unmap and close. Safe to call with a zeroed MmapFile. */
void mmap_close(MmapFile *mf);

#endif
