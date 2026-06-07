import fastcsv, numpy as np, tempfile, os, sys

passed = 0
failed = 0

def test(name, cond):
    global passed, failed
    if cond:
        print(f"  PASS  {name}")
        passed += 1
    else:
        print(f"  FAIL  {name}")
        failed += 1

def csv(content):
    f = tempfile.NamedTemporaryFile(mode='w', suffix='.csv', delete=False, encoding='utf-8')
    f.write(content)
    f.close()
    return f.name

# Version
test("version string exists", isinstance(fastcsv.__version__, str))

# read_csv return type
p = csv("a,b\n1,2\n")
r = fastcsv.read_csv(p)
test("returns dict", isinstance(r, dict))
os.unlink(p)

# int column
p = csv("x\n1\n2\n3\n")
r = fastcsv.read_csv(p)
test("int dtype", r['x'].dtype == np.int64)
test("int values", [str(x) if hasattr(x, "as_py") else x for x in r['x']] == [1, 2, 3])
os.unlink(p)

# float column
p = csv("x\n1.1\n2.2\n")
r = fastcsv.read_csv(p)
test("float dtype", r['x'].dtype == np.float64)
os.unlink(p)

# str column
p = csv("x\nhello\nworld\n")
r = fastcsv.read_csv(p)
test("str values", [str(x) if hasattr(x, "as_py") else x for x in r['x']] == ['hello', 'world'])
os.unlink(p)

# mixed downgrades to float
p = csv("x\n1\n2.5\n3\n")
r = fastcsv.read_csv(p)
test("int+float -> float64", r['x'].dtype == np.float64)
os.unlink(p)

# no header
p = csv("1,2\n3,4\n")
r = fastcsv.read_csv(p, has_header=False)
test("no header col_0", 'col_0' in r)
test("no header col_1", 'col_1' in r)
os.unlink(p)

# tab delimiter
p = csv("a\tb\n1\t2\n")
r = fastcsv.read_csv(p, delimiter='\t')
test("tab delimiter", [str(x) if hasattr(x, "as_py") else x for x in r['a']] == [1])
os.unlink(p)

# quoted field
p = csv('a\n"hello, world"\n')
r = fastcsv.read_csv(p)
test("quoted field with comma", str(r['a'][0]) == 'hello, world')
os.unlink(p)


# BOM
p = csv("\ufeffa,b\n1,2\n")
r = fastcsv.read_csv(p)
test("BOM stripped from header", 'a' in r)
os.unlink(p)

# strict error
p = csv('"unclosed\n')
try:
    fastcsv.read_csv(p, error_mode='strict')
    test("strict raises", False)
except ValueError:
    test("strict raises ValueError", True)
os.unlink(p)

# reader chunks
p = csv("a\n" + "\n".join(str(i) for i in range(100)) + "\n")
chunks = list(fastcsv.reader(p, chunk_size=30))
test("reader chunk count", len(chunks) == 4)
test("reader total rows", sum(len(c['a']) for c in chunks) == 100)
os.unlink(p)

# simd width reported
width = fastcsv.fastcsv.simd_width() if hasattr(fastcsv.fastcsv, 'simd_width') else 1
test("simd width is 1, 16, or 32", width in (1, 16, 32))

print(f"\n{passed} passed, {failed} failed")
sys.exit(0 if failed == 0 else 1)
