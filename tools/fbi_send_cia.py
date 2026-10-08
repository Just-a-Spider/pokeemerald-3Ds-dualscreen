#!/usr/bin/env python3
import os
import socket
import struct
import sys
import threading
import time
from urllib.parse import quote
from http.server import SimpleHTTPRequestHandler
from socketserver import TCPServer

class ReusableTCPServer(TCPServer):
    allow_reuse_address = True

def main():
    target_ip = sys.argv[1] if len(sys.argv) > 1 else "10.202.80.185"
    target_port = 5000
    cia_path = sys.argv[2] if len(sys.argv) > 2 else "emerald3ds.cia"
    
    if not os.path.isfile(cia_path):
        print(f"File not found: {cia_path}")
        return 1

    cia_path = os.path.abspath(cia_path)
    cia_dir = os.path.dirname(cia_path)
    cia_file = os.path.basename(cia_path)

    # Determine host IP by routing to target IP
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((target_ip, target_port))
        host_ip = s.getsockname()[0]
    finally:
        s.close()

    host_port = 8080
    print(f"Serving {cia_file} from {host_ip}:{host_port}...")

    # Start HTTP server in file's directory
    os.chdir(cia_dir)
    httpd = ReusableTCPServer(('', host_port), SimpleHTTPRequestHandler)
    server_thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    server_thread.start()

    payload = f"{host_ip}:{host_port}/{quote(cia_file)}"
    payload_bytes = payload.encode('ascii')

    print(f"Connecting to 3DS at {target_ip}:{target_port}...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(300.0)
    sock.connect((target_ip, target_port))

    print(f"Sending install payload: {payload}")
    sock.sendall(struct.pack('!I', len(payload_bytes)) + payload_bytes)
    print("Payload sent. FBI downloading and installing CIA on console...")

    # Wait for completion confirmation byte from FBI
    try:
        data = sock.recv(1)
        if data:
            print("FBI install completed successfully!")
        else:
            print("Connection closed by 3DS.")
    except Exception as e:
        print(f"Socket wait result: {e}")
    finally:
        sock.close()
        httpd.shutdown()

    return 0

if __name__ == '__main__':
    sys.exit(main())
