import time, os, gc
import fastcsv
import pandas as pd
import polars as pl

FILES = {
    "1M rows mixed":   "bench/data/data_1m.csv",
    "100k rows wide":  "bench/data/data_wide.csv",
    "500k rows str":   "bench/data/data_str.csv",
}

RUNS = 3

def bench(name, fn):
    times = []
    for _ in range(RUNS):
        gc.collect()
        t = time.perf_counter()
        fn()
        times.append(time.perf_counter() - t)
    best = min(times)
    return best

def file_mb(path):
    return os.path.getsize(path) / 1024 / 1024

print(f"\n{'File':<22} {'Library':<12} {'Best(s)':>8} {'MB/s':>10}")
print("─" * 56)

for label, path in FILES.items():
    mb = file_mb(path)
    print(f"Running {label}..."); results = {
        "fastcsv":  bench(label, lambda p=path: fastcsv.read_csv(p)),
        "pandas":   bench(label, lambda p=path: pd.read_csv(p)),
        "polars":   bench(label, lambda p=path: pl.read_csv(p)),
    }
    for lib, t in results.items():
        mbs = mb / t
        print(f"{label:<22} {lib:<12} {t:>8.3f} {mbs:>9.0f} MB/s")
    print()

# Winner per file
print("── Winner per file ──")
for label, path in FILES.items():
    mb = file_mb(path)
    times = {
        "fastcsv": bench(label, lambda p=path: fastcsv.read_csv(p)),
        "pandas":  bench(label, lambda p=path: pd.read_csv(p)),
        "polars":  bench(label, lambda p=path: pl.read_csv(p)),
    }
    winner = min(times, key=times.get)
    print(f"  {label}: {winner} ({mb/times[winner]:.0f} MB/s)")
