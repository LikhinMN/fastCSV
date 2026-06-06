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

test-types:
	$(CC) $(CFLAGS) src/type_infer.c tests/test_type_infer.c -o test_types && ./test_types

test-columnar:
	$(CC) $(CFLAGS) src/columnar.c src/type_infer.c tests/test_columnar.c -o test_columnar && ./test_columnar

test: test-mmap test-parser test-types test-columnar

bench-data:
	python bench/gen_data.py

bench: bench-data
	python bench/bench.py

test-robustness:
	python tests/test_robustness.py

test-all: test bench-data
	python tests/test_python.py
	python tests/test_robustness.py

sdist:
	python -m build --sdist

wheel:
	python -m build --wheel

install-dev:
	pip install -e ".[bench]"

clean:
	rm -f test_mmap test_runner test_types test_columnar bench_simd
	rm -rf build dist *.egg-info fastcsv/*.so fastcsv/*.pyd
