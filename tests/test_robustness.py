import fastcsv, tempfile, os

def write_csv(content, mode='w', encoding='utf-8'):
    f = tempfile.NamedTemporaryFile(mode=mode, suffix='.csv',
                                    delete=False, encoding=encoding)
    f.write(content)
    f.close()
    return f.name

def test(name, cond):
    print(f"  {'PASS' if cond else 'FAIL'}  {name}")

# BOM
p = write_csv("\ufeffa,b\n1,2\n")
r = fastcsv.read_csv(p)
test("BOM: header col not prefixed with BOM", 'a' in r and 'b' in r)
os.unlink(p)

# Empty file
p = write_csv("")
r = fastcsv.read_csv(p, has_header=False)
test("empty file: returns empty dict", isinstance(r, dict))
os.unlink(p)

# Single column
p = write_csv("val\n1\n2\n3\n")
r = fastcsv.read_csv(p)
test("single col: correct values", list(r['val']) == [1, 2, 3])
os.unlink(p)

# 50-column wide file
row = ','.join(str(i) for i in range(50))
header = ','.join(f'c{i}' for i in range(50))
p = write_csv(header + '\n' + '\n'.join([row]*1000) + '\n')
r = fastcsv.read_csv(p)
test("wide: 50 cols present", len(r) == 50)
test("wide: 1000 rows", len(r['c0']) == 1000)
os.unlink(p)

# reader: break early (tests tp_dealloc)
p = write_csv("a\n" + "\n".join(str(i) for i in range(10000)) + "\n")
it = fastcsv.reader(p, chunk_size=100)
chunk = next(it)
del it  # must not crash or leak
test("reader: early break no crash", True)
os.unlink(p)

# error_mode strict: malformed raises ValueError
p = write_csv('"unclosed,b\n')
raised = False
try:
    fastcsv.read_csv(p, error_mode="strict")
except ValueError:
    raised = True
test("strict: malformed raises ValueError", raised)
os.unlink(p)

# error_mode replace: malformed returns result
p = write_csv('"unclosed,b\n')
try:
    r = fastcsv.read_csv(p, error_mode="replace")
    test("replace: malformed returns dict", isinstance(r, dict))
except Exception:
    test("replace: malformed returns dict", False)
os.unlink(p)

# mixed int/float column downgrades correctly
p = write_csv("x\n1\n2\n3.5\n4\n")
r = fastcsv.read_csv(p)
test("mixed int/float: dtype float64", r['x'].dtype.name == 'float64')
os.unlink(p)

# large quoted field with embedded newlines
inner = "line1\nline2\nline3"
p = write_csv(f'text\n"{inner}"\n')
r = fastcsv.read_csv(p)
test("multiline quoted field: correct value", str(r['text'][0]) == inner)
os.unlink(p)

print("Done.")
