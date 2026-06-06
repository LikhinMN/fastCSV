import csv, random, string, os

random.seed(42)
os.makedirs("bench/data", exist_ok=True)

def rand_str(n=8):
    return ''.join(random.choices(string.ascii_lowercase, k=n))

# 1. data_1m.csv — 1 million rows, mixed types
# columns: id(int), value(float), category(str), score(int), label(str)
with open("bench/data/data_1m.csv", "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["id", "value", "category", "score", "label"])
    for i in range(1_000_000):
        w.writerow([
            i,
            round(random.uniform(0, 10000), 4),
            rand_str(6),
            random.randint(0, 100),
            rand_str(10),
        ])

# 2. data_wide.csv — 100k rows, 50 columns (all int)
cols = [f"col_{i}" for i in range(50)]
with open("bench/data/data_wide.csv", "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(cols)
    for _ in range(100_000):
        w.writerow([random.randint(0, 999999) for _ in range(50)])

# 3. data_str.csv — 500k rows, all string columns (worst case for any parser)
with open("bench/data/data_str.csv", "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["first", "last", "email", "city"])
    for _ in range(500_000):
        w.writerow([rand_str(6), rand_str(8),
                    rand_str(10)+"@example.com", rand_str(7)])

print("Generated:")
for name in ["data_1m.csv", "data_wide.csv", "data_str.csv"]:
    size = os.path.getsize(f"bench/data/{name}") / 1024 / 1024
    print(f"  bench/data/{name}  {size:.1f} MB")
