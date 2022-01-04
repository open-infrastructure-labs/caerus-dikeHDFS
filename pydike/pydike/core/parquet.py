import time
import threading
import fastparquet
from fastparquet import core

from pydike.core.webhdfs import WebHdfsFile

import pyarrow.parquet
import numpy

import cProfile
import pstats
from pstats import SortKey


reader_cache = dict()
reader_cache_lock = threading.Lock()


def get_reader(path: str):
    reader_cache_lock.acquire()
    if path not in reader_cache.keys():
        # reader_cache[path] = ParquetReader(path)
        reader_cache[path] = ArrowParquetReader(path)
    reader_cache_lock.release()

    return reader_cache[path]


class ParquetReader(fastparquet.ParquetFile):
    def __init__(self, path: str):
        self.infile = WebHdfsFile(path)
        super(ParquetReader, self).__init__(self.infile)
        self.dtypes = self.dtypes.values()
        self.num_row_groups = len(self.row_groups)
        self.infile.close()

    def read_rg(self, index: int, columns: list):
        rg = self.row_groups[index]
        #  Allocate memory
        df, assign = self.pre_allocate(rg.num_rows, columns, {}, None)

        fin = self.infile.clone()

        for col in rg.columns:
            name = col.meta_data.path_in_schema[0]
            if name in columns:
                off = min((col.meta_data.dictionary_page_offset or col.meta_data.data_page_offset,
                           col.meta_data.data_page_offset))
                fin.prefetch(off, col.meta_data.total_compressed_size + 65536)
                core.read_col(col, self.schema, fin, assign=assign[name])

        fin.close()
        del fin
        return df


class ArrowParquetReader(pyarrow.parquet.ParquetFile):
    def __init__(self, path: str):
        self.infile = WebHdfsFile(path)
        super(ArrowParquetReader, self).__init__(self.infile)
        self.columns = self.metadata.schema.names
        self.dtypes = [numpy.dtype(d.to_pandas_dtype()) for d in self.schema_arrow.types]
        # self.num_row_groups = self.metadata.num_row_groups
        self.infile.close()

    def read_rg(self, index: int, columns: list):
        fin = self.infile.clone()
        pfin = pyarrow.parquet.ParquetFile(fin, metadata=self.metadata)
        df = pfin.read_row_group(index, columns=columns)
        fin.close()
        return df


if __name__ == '__main__':
    fname = '/tpch-test-parquet-1g/lineitem.parquet/' \
            'part-00000-badcef81-d816-44c1-b936-db91dae4c15f-c000.snappy.parquet'

    reader = get_reader(f'webhdfs://dikehdfs:9870/{fname}?user.name=peter')
    print(reader.columns)
    dtypes = [t.name for t in reader.dtypes]
    print(dtypes)
    print(reader.num_row_groups)
    start = time.time()
    columns = ['l_partkey', 'l_extendedprice', 'l_discount', 'l_shipdate']

    df = reader.read_rg(0, columns[:4])
    # cProfile.run('df = reader.read_rg(0, columns[:4])', 'profile.txt')
    # p = pstats.Stats('profile.txt')
    # p.sort_stats(SortKey.CUMULATIVE).print_stats(10)
    # p.sort_stats(SortKey.TIME).print_stats(2)
    # p.print_callees(10)

    end = time.time()
    print(f"Run time is: {end - start:.3f} secs")
    print(df.shape)

    # gprof2dot.py -f pstats ./pydike/pydike/core/profile.txt| dot -Tpng -o output.png