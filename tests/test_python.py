import fastcsv, numpy as np, tempfile, os

def write_csv(content):
    f = tempfile.NamedTemporaryFile(mode='w', suffix='.csv', delete=False)
    f.write(content)
    f.close()
    return f.name

def test(name, cond):
    print(f"  {'PASS' if cond else 'FAIL'}  {name}")

# read_csv basic
p = write_csv("a,b,c\n1,2,3\n4,5,6\n")
r = fastcsv.read_csv(p)
test("read_csv: returns dict", isinstance(r, dict))
test("read_csv: 3 keys", len(r) == 3)
test("read_csv: col a dtype int", r['a'].dtype == np.int64)
test("read_csv: col a values", [str(x) if hasattr(x, "as_py") else x for x in r['a']] == [1, 4])
os.unlink(p)

# no header
p = write_csv("1,2,3\n4,5,6\n")
r = fastcsv.read_csv(p, has_header=False)
test("no header: col_0 exists", 'col_0' in r)
os.unlink(p)

# float column
p = write_csv("x\n1.5\n2.5\n")
r = fastcsv.read_csv(p)
test("float col dtype", r['x'].dtype == np.float64)
os.unlink(p)

# string column
p = write_csv("name\nalice\nbob\n")
r = fastcsv.read_csv(p)
test("str col values", [str(x) if hasattr(x, "as_py") else x for x in r['name']] == ['alice', 'bob'])
os.unlink(p)

# reader iterator
p = write_csv("a,b\n" + "1,2\n" * 25)
chunks = list(fastcsv.reader(p, chunk_size=10))
test("reader: correct chunk count", len(chunks) == 3)
test("reader: last chunk size", len(chunks[-1]['a']) == 5)
os.unlink(p)

print("Done.")
