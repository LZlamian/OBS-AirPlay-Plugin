# SPDX-License-Identifier: GPL-3.0-or-later
# HTTPS file server with a self-signed certificate (tests/with-media-servers.sh).
import http.server, os, ssl, sys
directory, port = sys.argv[1], int(sys.argv[2])
os.chdir(directory)
server = http.server.ThreadingHTTPServer(('127.0.0.1', port), http.server.SimpleHTTPRequestHandler)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain('cert.pem', 'key.pem')
server.socket = context.wrap_socket(server.socket, server_side=True)
server.serve_forever()
