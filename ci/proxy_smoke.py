"""Online smoke test for an installed ruyijs wheel: one page load through a proxy.

Starts a minimal HTTP proxy on 127.0.0.1 that only tunnels CONNECT, opens
https://example.com/ with ``Context(proxy=...)`` and checks the title and that
the proxy saw ``CONNECT example.com:443``. Exit status is non-zero when the page
did not load or did not go through the proxy.
"""
import socket
import socketserver
import sys
import threading
import time

from ruyijs import Context

URL = "https://example.com/"
SEEN = []


def relay(source, target):
    try:
        while True:
            data = source.recv(65536)
            if not data:
                break
            target.sendall(data)
    except OSError:
        pass
    finally:
        for sock in (source, target):
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


class Tunnel(socketserver.BaseRequestHandler):
    def handle(self):
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.request.recv(4096)
            if not chunk or len(head) > 65536:
                return
            head += chunk
        line, _, rest = head.partition(b"\r\n")
        rest = rest.partition(b"\r\n\r\n")[2]
        method, target, _ = line.decode("latin-1").split(" ", 2)
        SEEN.append(f"{method} {target}")
        if method != "CONNECT":
            self.request.sendall(b"HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n")
            return
        host, _, port = target.rpartition(":")
        try:
            upstream = socket.create_connection((host.strip("[]"), int(port)), timeout=30)
        except OSError:
            self.request.sendall(b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n")
            return
        self.request.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
        if rest:
            upstream.sendall(rest)
        back = threading.Thread(target=relay, args=(upstream, self.request), daemon=True)
        back.start()
        relay(self.request, upstream)
        back.join(30)
        upstream.close()


class Proxy(socketserver.ThreadingTCPServer):
    daemon_threads = True


def main():
    with Proxy(("127.0.0.1", 0), Tunnel) as proxy:
        threading.Thread(target=proxy.serve_forever, daemon=True).start()
        address = f"http://127.0.0.1:{proxy.server_address[1]}"
        with Context(timeout=120, proxy=address) as ctx:
            ctx.configure_clock("system")
            ctx.navigate(URL)
            deadline = time.monotonic() + 90
            title = ""
            while time.monotonic() < deadline:
                ctx.advance(250)
                if ctx.eval("document.readyState") == "complete":
                    title = ctx.eval("document.title")
                    break
            requests = [(entry.get("method"), entry.get("status"), entry.get("url")) for entry in ctx.net_log()]
        proxy.shutdown()
    print("requests:", requests[:5])
    print("proxy saw:", SEEN[:5])
    print("title:", repr(title))
    if "Example Domain" not in title:
        print("FAIL: the page did not load through the proxy")
        sys.exit(1)
    if "CONNECT example.com:443" not in SEEN:
        print("FAIL: the page did not go through the proxy")
        sys.exit(1)
    print("proxy smoke check passed")


if __name__ == "__main__":
    main()
