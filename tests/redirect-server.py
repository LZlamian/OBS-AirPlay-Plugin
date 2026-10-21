# SPDX-License-Identifier: GPL-3.0-or-later
# HTTP server for the player's fetch checks (tests/with-media-servers.sh),
# on 127.0.0.1 and [::1]. It records the Host header and path of every
# request, so a test can tell what was requested and under which name:
#   /files/NAME          DIRECTORY/NAME (Range requests honoured)
#   /hls/NAME            DIRECTORY/hls/NAME
#   /redirect/NAME       302 to /files/NAME
#   /redirect-hls/NAME   302 to /hls/NAME
#   /redirect-out/NAME   302 to http://[::1]:PORT/secret/NAME
#   /log                 the requests so far, one "HOST PATH" per line
#   redirect-server.py DIRECTORY PORT
import http.server, os, re, socket, sys, threading

directory, port = sys.argv[1], int(sys.argv[2])
log, log_lock = [], threading.Lock()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def redirect(self, location):
        self.send_response(302)
        self.send_header('Location', location)
        self.send_header('Content-Length', '0')
        self.end_headers()

    def send_file(self, path, content_type):
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
        self.send_header('Content-Type', content_type)
        self.send_header('Accept-Ranges', 'bytes')
        self.send_header('Content-Length', str(end - start + 1))
        self.end_headers()
        with open(path, 'rb') as f:
            f.seek(start)
            self.wfile.write(f.read(end - start + 1))

    def do_GET(self):
        path = self.path.split('?')[0]
        name = os.path.basename(path)
        if path == '/log':
            with log_lock:
                body = ''.join('%s %s\n' % entry for entry in log).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        with log_lock:
            log.append((self.headers.get('Host', '-'), path))
        if path.startswith('/files/'):
            self.send_file(os.path.join(directory, name), 'video/mp4')
        elif path.startswith('/hls/'):
            content_type = ('application/vnd.apple.mpegurl' if name.endswith('.m3u8')
                            else 'video/mp2t')
            self.send_file(os.path.join(directory, 'hls', name), content_type)
        elif path.startswith('/redirect/'):
            self.redirect('/files/' + name)
        elif path.startswith('/redirect-hls/'):
            self.redirect('/hls/' + name)
        elif path.startswith('/redirect-out/'):
            self.redirect('http://[::1]:%d/secret/%s' % (port, name))
        else:
            self.send_error(404)


class Server6(http.server.ThreadingHTTPServer):
    address_family = socket.AF_INET6


servers = [http.server.ThreadingHTTPServer(('127.0.0.1', port), Handler),
           Server6(('::1', port), Handler)]
for server in servers[1:]:
    threading.Thread(target=server.serve_forever, daemon=True).start()
servers[0].serve_forever()
