#include <stdio.h>
#include <assert.h>
#include "../src/mmap_io.h"

int main(void) {
    /* Write a temp file, mmap it, check contents, close it */
    FILE *f = fopen("tests/tmp_mmap.csv", "w");
    fprintf(f, "hello,world\n");
    fclose(f);

    MmapFile mf;
    int rc = mmap_open("tests/tmp_mmap.csv", &mf);
    assert(rc == 0);
    assert(mf.len == 12);
    assert(mf.data[0] == 'h');
    assert(mf.data[5] == ',');
    mmap_close(&mf);

    /* Double-close must not crash */
    mmap_close(&mf);

    /* Non-existent file must return -1 */
    rc = mmap_open("tests/no_such_file.csv", &mf);
    assert(rc == -1);

    printf("mmap smoke test PASSED\n");
    return 0;
}
