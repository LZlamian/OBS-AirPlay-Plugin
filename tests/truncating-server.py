# SPDX-License-Identifier: GPL-3.0-or-later
# HTTP file server that behaves like a sender media server dropping
# responses (tests/with-media-servers.sh): it honours Range requests and
# announces the full length, but closes every response after CHUNK bytes.
#   truncating-server.py DIRECTORY PORT CHUNK
import http.server, os, re, sys

directory, port, chunk = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_GET(self):
        path = os.path.join(directory, os.path.basename(self.path.split('?')[0]))
        if not os.path.isfile(path):
            self.send_error(404)
            return
        size = os.path.getsize(path)
        start, end = 0, size - 1
        match = re.match(r'bytes=(\d+)-(\d*)', self.headers.get('Range', ''))
        if match:
            start = int(match.group(1))
            if match.group(2):
                end = min(int(match.group(2)), size - 1)
            if start >= size:
                self.send_response(416)
                self.send_header('Content-Range', 'bytes */%d' % size)
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            self.send_response(206)
            self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, end, size))
        else:
            self.send_response(200)
        self.send_header('Content-Type', 'video/mp4')
        self.send_header('Accept-Ranges', 'bytes')
        self.send_header('Content-Length', str(end - start + 1))
        self.send_header('Connection', 'close')
        self.end_headers()
        with open(path, 'rb') as f:
            f.seek(start)
            self.wfile.write(f.read(min(chunk, end - start + 1)))
        self.close_connection = True


http.server.ThreadingHTTPServer(('127.0.0.1', port), Handler).serve_forever()
