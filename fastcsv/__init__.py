try:
    import pyarrow as pa
    _HAS_ARROW = True
except ImportError:
    _HAS_ARROW = False

from .fastcsv import read_csv as _read_csv_c, reader

__version__ = "0.1.0"


def read_csv(path, delimiter=",", has_header=True, error_mode="strict", raw=False):
    """Read an entire CSV file into a dict of numpy arrays.

    When pyarrow is available, string columns are returned as
    pyarrow.LargeStringArray (zero-copy, lazy string creation).
    Otherwise they are returned as numpy object arrays of Python str.
    """
    use_arrow = _HAS_ARROW and not raw
    result = _read_csv_c(
        path,
        delimiter=delimiter,
        has_header=has_header,
        error_mode=error_mode,
        _arrow_strings=use_arrow,
        raw=raw
    )
    if use_arrow:
        for key in list(result.keys()):
            val = result[key]
            if isinstance(val, tuple) and len(val) == 2:
                offsets, data = val
                buf_offsets = pa.py_buffer(offsets)
                buf_data = pa.py_buffer(data)
                n = len(offsets) - 1
                result[key] = pa.LargeStringArray.from_buffers(
                    n, buf_offsets, buf_data
                )
    return result


__all__ = ["read_csv", "reader", "__version__"]
