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
| 1M rows mixed   | 0.053s   | 1.086s   | 0.034s   |
| 100k rows wide  | 0.027s   | 0.380s   | 0.033s   |
| 500k rows str   | 0.020s   | 0.935s   | 0.015s   |
