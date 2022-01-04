import os
import time
import urllib.parse
import http.client
import json


class WebHdfsFile(object):
    def __init__(self, name, user=None, buffer_size=(128 << 10), orig=None):
        self.name = name
        self.user = user
        self.buffer_size = buffer_size
        self.closed = False
        self.offset = 0
        self.read_stats = []
        self.read_bytes = 0
        self.verbose = False
        self.prefetch_buffer = None
        self.prefetch_offset = -1
        self.prefetch_length = -1

        self.url = urllib.parse.urlparse(self.name)
        if self.user is None:
            for q in self.url.query.split('&'):
                if 'user.name=' in q:
                    self.user = q.split('user.name=')[1]

        if orig is None:
            self.size = self.get_size()
        else:
            self.size = orig.size
            self.offset = orig.offset

        if orig is None:
            conn = http.client.HTTPConnection(self.url.netloc)
            req = f'/webhdfs/v1{self.url.path}?op=OPEN&user.name={self.user}&buffersize={self.buffer_size}'
            conn.request("GET", req)
            resp = conn.getresponse()
            self.data_url = urllib.parse.urlparse(resp.headers['Location'])
            conn.close()
        else:
            self.data_url = orig.data_url

        query = self.data_url.query.split('&offset=')[0]
        self.data_req = f'{self.data_url.path}?{query}'

    def clone(self):
        f = WebHdfsFile(self.name, user=self.user, buffer_size=self.buffer_size, orig=self)
        return f

    def get_size(self):
        conn = http.client.HTTPConnection(self.url.netloc)
        req = f'/webhdfs/v1/{self.url.path}?op=GETFILESTATUS&user.name={self.user}'
        conn.request("GET", req)
        resp = conn.getresponse()
        resp_data = resp.read()
        file_status = json.loads(resp_data)['FileStatus']
        conn.close()
        return file_status['length']

    def seek(self, offset, whence=0):
        # print("Attempt to seek {} from {}".format(offset, whence))

        if whence == os.SEEK_SET:
            self.offset = offset
        elif whence == os.SEEK_CUR:
            self.offset += offset
        elif whence == os.SEEK_END:
            self.offset = self.size + offset

        return self.offset

    def prefetch(self, offset, length):
        print(f'Prefetch {offset} {length}')
        if self.prefetch_buffer is None or length > len(self.prefetch_buffer):
            # print(length)
            del self.prefetch_buffer
            self.prefetch_buffer = bytearray(max(20<<20, length))

        view = memoryview(self.prefetch_buffer)
        self.read_stats.append((offset, length))
        req = f'{self.data_req}&offset={offset}&length={length}'
        start = time.time()
        conn = http.client.HTTPConnection(self.data_url.netloc, blocksize=self.buffer_size)
        conn.request("GET", req)
        resp = conn.getresponse()
        resp.readinto(view[:length])
        conn.close()
        end = time.time()
        print(f"Read time is: {end - start:.3f} secs {length >> 20} MB at { (length >> 20) / (end - start):.3f} MB/s ")
        self.prefetch_offset = offset
        self.prefetch_length = length

    def read(self, length=-1):
        if self.verbose:
            print(f"Attempt to read from {self.offset} len {length}")

        pos = self.offset
        if length == -1:
            length = self.size - pos

        self.offset += length
        self.read_bytes += length
        if self.prefetch_offset >= 0:
            if pos >= self.prefetch_offset and (pos + length <= self.prefetch_offset + self.prefetch_length):
                begin = pos - self.prefetch_offset
                end = begin + length
                return self.prefetch_buffer[begin:end]

        req = f'{self.data_req}&offset={pos}&length={length}'
        # start = time.time()
        conn = http.client.HTTPConnection(self.data_url.netloc, blocksize=self.buffer_size)
        conn.request("GET", req)
        resp = conn.getresponse()
        data = resp.read(length)
        conn.close()
        # print(f'Prefetch miss {pos}, {length},  {self.prefetch_offset},{self.prefetch_length}')
        # end = time.time()
        # print(f"Prefetch miss Read time is: {end - start:.3f} secs {length >> 20} MB at {(length >> 20) / (end - start):.3f} MB/s ")

        return data

    def readinto(self, b):
        buffer = self.read(len(b))
        length = len(buffer)
        b[:length] = buffer
        return length

    def tell(self):
        return self.offset

    def seekable(self):
        return True

    def readable(self):
        return True

    def writable(self):
        return False

    def close(self):
        del self.prefetch_buffer
        self.prefetch_buffer = None

