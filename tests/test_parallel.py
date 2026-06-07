import fastcsv, numpy as np, tempfile, os

def csv(content):
    f = tempfile.NamedTemporaryFile(mode='w', suffix='.csv', delete=False)
    f.write(content)
    f.close()
    return f.name

def test(name, cond):
    print(f"  {'PASS' if cond else 'FAIL'}  {name}")

# Correctness: parallel result matches single-threaded
import pandas as pd
p = csv("a,b,c\n" + "\n".join(f"{i},{i*2},{i*3}" for i in range(10000)) + "\n")
r = fastcsv.read_csv(p)
ref = pd.read_csv(p)
test("parallel: row count correct", len(r['a']) == 10000)
test("parallel: col a matches pandas", list(r['a']) == list(ref['a']))
test("parallel: col c matches pandas", list(r['c']) == list(ref['c']))
os.unlink(p)

# Large mixed file
p = csv("x,y,z\n" + "\n".join(f"{i},{i/3:.4f},str{i}" for i in range(100000)) + "\n")
r = fastcsv.read_csv(p)
test("parallel large: row count", len(r['x']) == 100000)
test("parallel large: int col", r['x'].dtype == np.int64)
test("parallel large: float col", r['y'].dtype == np.float64)
os.unlink(p)

# Tiny file stays correct (single-threaded fallback)
p = csv("a\n1\n2\n3\n")
r = fastcsv.read_csv(p)
test("tiny file: correct", list(r['a']) == [1, 2, 3])
os.unlink(p)

print("Done.")
