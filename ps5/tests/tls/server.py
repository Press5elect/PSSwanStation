#!/usr/bin/env python3
# PSSwanStation - the local HTTPS server tests/tls/run.sh tests tls_client.cpp against.
#   server.py PORT 1.2|1.3   (srv.pem, srv.key and big<PORT>.bin in the working folder)
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
import ssl, sys, os, http.server, socketserver
port=int(sys.argv[1]); ver=sys.argv[2]
big=os.urandom(3*1024*1024+123)
open('big%d.bin'%port,'wb').write(big)
class H(http.server.BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def log_message(self,*a): pass
    def send_body(self,code,body,ct='text/plain'):
        self.send_response(code); self.send_header('Content-Type',ct); self.send_header('Content-Length',str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        p=self.path
        if p.startswith('/ok'): self.send_body(200,b'hello from '+p.encode()+b' ua='+self.headers.get('User-Agent','').encode())
        elif p=='/chunked':
            self.send_response(200); self.send_header('Transfer-Encoding','chunked'); self.end_headers()
            for part in [b'chunk-one,',b'chunk-two,',b'x'*20000]:
                self.wfile.write(b'%x\r\n'%len(part)+part+b'\r\n')
            self.wfile.write(b'0\r\n\r\n')
        elif p=='/redirect': self.send_response(302); self.send_header('Location','https://localhost:%d/ok?via=abs'%port); self.send_header('Content-Length','0'); self.end_headers()
        elif p=='/rel': self.send_response(301); self.send_header('Location','/ok?via=rel'); self.send_header('Content-Length','0'); self.end_headers()
        elif p=='/missing': self.send_body(404,b'{"Error":"not here"}')
        elif p=='/big': self.send_body(200,big,'application/octet-stream')
        elif p=='/bigchunked':
            self.send_response(200); self.send_header('Transfer-Encoding','chunked'); self.end_headers()
            for i in range(0,len(big),70000):
                part=big[i:i+70000]; self.wfile.write(b'%x\r\n'%len(part)+part+b'\r\n')
            self.wfile.write(b'0\r\n\r\n')
        elif p=='/close':
            self.send_response(200); self.send_header('Connection','close'); self.end_headers(); self.wfile.write(b'until-close'); self.close_connection=True
class S(socketserver.ThreadingMixIn, http.server.HTTPServer): daemon_threads=True
ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain('srv.pem','srv.key')
if ver=='1.2': ctx.maximum_version=ssl.TLSVersion.TLSv1_2
else: ctx.minimum_version=ssl.TLSVersion.TLSv1_3
srv=S(('127.0.0.1',port),H); srv.socket=ctx.wrap_socket(srv.socket,server_side=True); srv.serve_forever()
