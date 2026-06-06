CC     = gcc
CFLAGS = -O2 -Wall -Wextra -std=c11
SIMD_CFLAGS = -O3 -Wall -Wextra -std=c11

test-mmap:
	$(CC) $(CFLAGS) src/mmap_io.c tests/test_mmap.c -o test_mmap && ./test_mmap

test-parser:
	$(CC) $(SIMD_CFLAGS) src/parser.c src/mmap_io.c src/simd.c \
	      tests/test_parser.c -o test_runner && ./test_runner

bench-simd:
	$(CC) $(SIMD_CFLAGS) src/parser.c src/mmap_io.c src/simd.c \
	      tests/bench_simd.c -o bench_simd && ./bench_simd

test: test-mmap test-parser

clean:
	rm -f test_mmap test_runner bench_simd
