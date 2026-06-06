CC     = gcc
CFLAGS = -O2 -Wall -Wextra -std=c11

test-mmap:
	$(CC) $(CFLAGS) src/mmap_io.c tests/test_mmap.c -o test_mmap && ./test_mmap

test-parser:
	$(CC) $(CFLAGS) src/parser.c src/mmap_io.c tests/test_parser.c -o test_runner && ./test_runner

test: test-mmap test-parser

clean:
	rm -f test_mmap test_runner
