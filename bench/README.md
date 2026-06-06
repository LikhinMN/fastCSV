## Benchmark files

- data_1m.csv — 1M rows, mixed int/float/str. Primary benchmark.
- data_wide.csv — 100k rows × 50 int columns. Tests columnar layout.
- data_str.csv — 500k rows, all strings. Tests worst-case allocation.

## How to run

    pip install pandas polars
    make bench

## Results

Hardware: AMD Ryzen 5 7235HS
OS:       Linux fedora 7.0.10-101.fc43.x86_64

| File            | fastcsv  | pandas   | polars   |
|-----------------|----------|----------|----------|
| 1M rows mixed   | 0.465s   | 0.962s   | 0.029s   |
| 100k rows wide  | 0.406s   | 0.403s   | 0.034s   |
| 500k rows str   | 0.192s   | 0.834s   | 0.012s   |
