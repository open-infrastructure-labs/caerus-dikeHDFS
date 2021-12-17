import time
from fastparquet import ParquetFile
from fastparquet import core

from pydike.core.webhdfs import WebHdfsFile


class ParquetReader(ParquetFile):
    def __init__(self, infile: WebHdfsFile):
        super(ParquetReader, self).__init__(infile)
        # self.pf = ParquetFile(infile)
        self.infile = infile

    def read_rg(self, index: int, columns: list):
        rg = self.row_groups[index]
        #  Allocate memory
        df, assign = self.pre_allocate(rg.num_rows, columns, {}, None)

        for col in rg.columns:
            name = col.meta_data.path_in_schema[0]
            if name in columns:
                self.infile.prefetch(col.meta_data.data_page_offset, col.meta_data.total_compressed_size)
                core.read_col(col, self.schema, self.infile, assign=assign[name])

        return df


if __name__ == '__main__':
    fname = '/tpch-test-parquet-1g/lineitem.parquet/' \
            'part-00000-badcef81-d816-44c1-b936-db91dae4c15f-c000.snappy.parquet'

    f = WebHdfsFile(f'webhdfs://dikehdfs:9870/{fname}', user='peter')
    reader = ParquetReader(f)
    print(reader.columns)
    print([t.name for t in reader.dtypes.values()])
    start = time.time()
    columns = ['l_partkey', 'l_extendedprice', 'l_discount', 'l_shipdate']
    df = reader.read_rg(0, columns[:4])

    # df = reader.read_rg(0, reader.pf.columns[:])
    end = time.time()
    print(f"Run time is: {end - start:.3f} secs {(f.read_bytes/(1<<20)) / (end - start):.3f} MB/s")
    print(df.shape)
