# fastCSV

The fastest CSV parser for Python. Written in C with AVX2 SIMD acceleration,
memory-mapped I/O, and zero-copy columnar NumPy output.

## Benchmarks

| File             | fastCSV  | pandas   | polars   |
|------------------|----------|----------|----------|
| 1M rows mixed    | 0.465s   | 0.962s   | 0.029s   |
| 100k rows wide   | 0.406s   | 0.403s   | 0.034s   |
| 500k rows str    | 0.192s   | 0.834s   | 0.012s   |

Hardware: AMD Ryzen 5 7235HS

## Installation

    pip install fastcsv

## Usage

```python
import fastcsv

# Returns dict of column_name -> numpy.ndarray
result = fastcsv.read_csv("data.csv")
result = fastcsv.read_csv("data.csv", delimiter=",", has_header=True, error_mode="strict")

# Streaming — constant memory regardless of file size
for chunk in fastcsv.reader("data.csv", chunk_size=10_000):
    process(chunk)
```

## Options

| Parameter | Default | Description |
|-----------|---------|-------------|
| `delimiter` | `","` | Field separator. Any single character. |
| `has_header` | `True` | Treat first row as column names. |
| `error_mode` | `"strict"` | `"strict"` / `"skip"` / `"replace"` |
| `chunk_size` | `10000` | Rows per chunk for `reader()`. |

## How it's fast

- **mmap I/O** — the OS pages in only what is needed. No `read()` syscalls.
- **AVX2 SIMD** — scans 32 bytes per cycle to find delimiters. Falls back to SSE4.2 (16 bytes) or scalar automatically.
- **Zero-copy output** — int and float columns are handed to NumPy directly from the C buffer. No data is copied.
- **Single-pass type inference** — column types resolved in one pass, never re-scanned.
- **GIL released** — the entire parse phase runs without the GIL. Safe for multi-threaded use.

## Error recovery

- `strict` — raises `ValueError` on any RFC 4180 violation.
- `skip` — silently drops malformed rows.
- `replace` — replaces malformed fields with empty string.

## Building from source

    pip install numpy
    pip install -e .
    make test-all

## License

MIT
904c2c43-028b-4195-83f8-cd7f0dcb5d2b