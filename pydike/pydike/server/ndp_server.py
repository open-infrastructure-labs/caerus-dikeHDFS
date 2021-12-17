#!/usr/bin/python3 -u
import threading
import argparse
import json
import numpy
import http.server
import http.client
from http import HTTPStatus
import urllib.parse

import pydike.core.webhdfs
import pydike.core.parquet
import pydike.client.tpch


class ChunkedWriter:
    def __init__(self, wfile):
        self.wfile = wfile

    def write(self, data):
        self.wfile.write(f'{len(data):x}\r\n'.encode())
        self.wfile.write(data)
        self.wfile.write('\r\n'.encode())

    def close(self):
        self.wfile.write('0\r\n\r\n'.encode())


logging_lock = threading.Lock()

class NdpRequestHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def log(self, msg):
        if self.server.config.verbose:
            logging_lock.acquire()
            print(msg)
            logging_lock.release()

    def send_name_node_request(self):
        conn = http.client.HTTPConnection(self.server.config.webhdfs)
        conn.request("GET", self.path, '', self.headers)
        response = conn.getresponse()
        data = response.read()
        conn.close()
        return response, data

    def parse_url(self):
        url = urllib.parse.urlparse(self.path)
        for q in url.query.split('&'):
            if 'user.name=' in q:
                user = q.split('user.name=')[1]
                setattr(self, 'user', user)
            if 'op=' in q:
                op = q.split('op=')[1]
                setattr(self, 'op', op)

    def do_POST(self):
        self.log(f'POST {self.path}')
        self.parse_url()
        data = self.rfile.read(int(self.headers['Content-Length']))
        config = json.loads(data)
        self.log(f'config {config}')

        url = urllib.parse.urlparse(config['url'])
        netloc = self.server.config.webhdfs
        config['url'] = f'http://{netloc}{url.path}?{url.query}'
        config['use_ndp'] = 'False'
        self.log(f'config.url {config["url"]}')

        config['verbose'] = self.server.config.verbose
        tpch_sql = pydike.client.tpch.TpchSQL(config)
        self.send_response(HTTPStatus.OK)
        self.send_header('Transfer-Encoding', 'chunked')
        self.end_headers()
        writer = ChunkedWriter(self.wfile)
        tpch_sql.to_spark(writer)
        writer.close()


    def do_GET(self):
        self.log(f'GET {self.path}')
        self.parse_url()

        if self.op == 'GETNDPINFO':
            return self.get_ndp_info()
        else:
            return self.forward_to_hdfs()

    def forward_to_hdfs(self):
        resp, data = self.send_name_node_request()
        self.send_response(resp.status, resp.reason)
        transfer_encoding = None
        for h in resp.headers.items():
            if h[0] == 'Transfer-Encoding':
                transfer_encoding = h[1]

            self.send_header(h[0], h[1])

        self.end_headers()
        if transfer_encoding == 'chunked':
            writer = ChunkedWriter(self.wfile)
            writer.write(data)
            writer.close()
        else:
            self.wfile.write(data)

        self.wfile.flush()

    def get_ndp_info(self):
        netloc = self.server.config.webhdfs
        f = pydike.core.webhdfs.WebHdfsFile(f'webhdfs://{netloc}/{self.path}', user=self.user)
        pf = pydike.core.parquet.ParquetReader(f)
        info = dict()
        info['columns'] = pf.columns
        #  info['dtypes'] = [numpy.dtype(c.to_pandas_dtype()).name for c in pf.schema_arrow.types]
        info['dtypes'] = [t.name for t in pf.dtypes.values()]
        info['num_row_groups'] = len(pf.row_groups)

        info_json = json.dumps(info)
        self.send_response(HTTPStatus.OK)
        self.end_headers()
        self.wfile.write(info_json.encode())


class NdpServer(http.server.ThreadingHTTPServer):
    def __init__(self, server_address, handler, config):
        super().__init__(server_address, handler)
        self.config = config


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Run NDP server.')
    parser.add_argument('-w', '--webhdfs', default='127.0.0.1:9870', help='Namenode http-address')
    parser.add_argument('-p', '--port', type=int, default='9860', help='Server port')
    parser.add_argument('-v', '--verbose', type=int, default='0', help='Verbose mode')
    config = parser.parse_args()
    print(f'Listening to port:{config.port} HDFS:{config.webhdfs}')
    ndp_server = NdpServer(('', config.port), NdpRequestHandler, config)
    try:
        ndp_server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        # Clean-up server (close socket, etc.)
        ndp_server.server_close()
