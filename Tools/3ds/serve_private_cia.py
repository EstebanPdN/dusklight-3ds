#!/usr/bin/env python3
"""Bounded local-only installer server: one explicitly selected CIA, no directory listing."""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, HTTPServer
import ipaddress
import json
from pathlib import Path
import re
import secrets
import socket
import time


def make_handler(payload, route):
    class InstallerHandler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.0'
        def log_message(self, *args): pass
        def do_HEAD(self): self.deliver(False)
        def do_GET(self): self.deliver(True)
        def deliver(self, body):
            if self.path != route:
                self.send_error(404); return
            size = len(payload); start, end, status = 0, size-1, 200
            requested = self.headers.get('Range')
            if requested:
                match = re.fullmatch(r'bytes=(\d*)-(\d*)',requested)
                if not match or not any(match.groups()):
                    self.send_error(416); return
                a,b=match.groups()
                if a:
                    start=int(a);end=min(int(b) if b else size-1,size-1)
                else:
                    suffix=int(b);start=max(0,size-suffix)
                    if not suffix: start=size
                if not 0 <= start <= end < size:
                    self.send_response(416);self.send_header('Content-Range',f'bytes */{size}');self.end_headers();return
                status=206
            self.send_response(status)
            self.send_header('Content-Type','application/octet-stream')
            self.send_header('Content-Length',str(end-start+1))
            self.send_header('Accept-Ranges','bytes')
            self.send_header('Cache-Control','no-store')
            self.send_header('Content-Disposition','attachment; filename="DuskLight.cia"')
            if status==206:self.send_header('Content-Range',f'bytes {start}-{end}/{size}')
            self.end_headers()
            if body:
                try:
                    for pos in range(start,end+1,65536):self.wfile.write(payload[pos:min(pos+65536,end+1)])
                except (BrokenPipeError,ConnectionResetError,TimeoutError):pass
        def setup(self):
            self.request.settimeout(10)
            super().setup()
    return InstallerHandler


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cia',type=Path,required=True);p.add_argument('--sha256',required=True)
    p.add_argument('--bind',required=True);p.add_argument('--port',type=int,default=8765)
    p.add_argument('--seconds',type=int,default=3600);p.add_argument('--receipt',type=Path,required=True)
    args=p.parse_args()
    address=ipaddress.ip_address(args.bind)
    if address.version!=4 or not address.is_private or address.is_unspecified or address.is_multicast:
        p.error('Use one explicit private IPv4 address')
    if not 1<=args.seconds<=3600 or not 1024<=args.port<=65535:p.error('Invalid duration or port')
    if args.cia.is_symlink() or not args.cia.is_file() or not 0<args.cia.stat().st_size<=64*1024*1024:
        p.error('Expected a bounded regular CIA')
    data=args.cia.read_bytes()
    if len(data)>64*1024*1024 or hashlib.sha256(data).hexdigest()!=args.sha256:p.error('CIA hash mismatch')
    route='/'+secrets.token_urlsafe(24)+'/DuskLight.cia'
    server=HTTPServer((args.bind,args.port),make_handler(data,route));server.timeout=1
    deadline=time.monotonic()+args.seconds
    record={'url':f'http://{args.bind}:{args.port}{route}','cia_sha256':args.sha256,
            'bytes':len(data),'expires_unix':time.time()+args.seconds,'scope':'one immutable installer only; no ISO, assets, dumps or directory listing'}
    try:
        with args.receipt.open('x') as output:json.dump(record,output,indent=2);output.write('\n')
        print(json.dumps(record),flush=True)
        while time.monotonic()<deadline:server.handle_request()
    except KeyboardInterrupt:pass
    finally:server.server_close()
if __name__=='__main__':main()
