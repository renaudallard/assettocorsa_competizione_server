"""Minimal fake kson lobby for accd integration tests.

Mirrors the on-the-wire side accServer.exe expects from the public
lobby at 131.153.158.178:909.  Used by run_lobby_*.sh after pointing
accd at it via ACCD_LOBBY_HOST=127.0.0.1 ACCD_LOBBY_PORT=NNNN.

Framing (after the 256 B init blob from accd):
    u16 LE length + body
Preamble (11 B): 0x3a + u8 type + u32 LE 7 + u32 LE 6 + u8 0

This server records every inbound frame and can inject any outbound
frame on demand; test scripts orchestrate the per-scenario sequence.
"""
import socket
import struct
import threading
import time


PREAMBLE_PROTO = 7
PREAMBLE_SID = 6


def write_kson_str(buf: bytearray, s: str) -> None:
    """u16 LE byte_len + N UTF-8 bytes."""
    b = s.encode("utf-8")
    buf.extend(struct.pack("<H", len(b)))
    buf.extend(b)


def build_preamble(msg_type: int) -> bytes:
    return bytes([0x3a, msg_type]) + struct.pack("<II", PREAMBLE_PROTO,
        PREAMBLE_SID) + b"\x00"


def frame(body: bytes) -> bytes:
    assert len(body) <= 0xffff
    return struct.pack("<H", len(body)) + body


class FakeKsonLobby:
    """One-shot server, single client.  Spawn, accept, run loop."""

    def __init__(self, port: int = 0):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.s.bind(("127.0.0.1", port))
        self.s.listen(1)
        self.port = self.s.getsockname()[1]
        self.client = None
        self.init_blob = b""
        self.inbox = []          # list of (type, body[1:]) tuples
        self.outbox_pending = []  # list of bytes to send before each recv
        self.lock = threading.Lock()
        self.stop = False
        self.thread = None

    def start(self) -> None:
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def send(self, body: bytes) -> None:
        """Queue a framed reply.  Goes out at the next recv tick."""
        with self.lock:
            self.outbox_pending.append(body)

    def _run(self) -> None:
        try:
            self.s.settimeout(5.0)
            self.client, _ = self.s.accept()
            self.client.settimeout(0.5)
        except Exception:
            return
        # Read 256 B init blob (unframed)
        buf = b""
        while len(buf) < 256 and not self.stop:
            try:
                chunk = self.client.recv(256 - len(buf))
            except socket.timeout:
                continue
            if not chunk:
                return
            buf += chunk
        self.init_blob = buf
        # Then loop framed messages
        stream = b""
        while not self.stop:
            with self.lock:
                pending = self.outbox_pending
                self.outbox_pending = []
            for body in pending:
                try:
                    self.client.sendall(frame(body))
                except Exception:
                    return
            try:
                chunk = self.client.recv(4096)
            except socket.timeout:
                continue
            except Exception:
                return
            if not chunk:
                return
            stream += chunk
            while len(stream) >= 2:
                n = struct.unpack("<H", stream[:2])[0]
                if len(stream) < 2 + n:
                    break
                body = stream[2:2 + n]
                stream = stream[2 + n:]
                if len(body) >= 11 and body[0] == 0x3a:
                    msg_type = body[1]
                    with self.lock:
                        self.inbox.append((msg_type, body[11:]))

    def shutdown(self) -> None:
        self.stop = True
        try:
            if self.client:
                self.client.close()
        except Exception:
            pass
        try:
            self.s.close()
        except Exception:
            pass

    def wait_for_type(self, msg_type: int, timeout: float = 5.0):
        """Return the (type, payload) for the first matching frame, or None."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                for entry in self.inbox:
                    if entry[0] == msg_type:
                        return entry
            time.sleep(0.05)
        return None

    def count(self, msg_type: int) -> int:
        with self.lock:
            return sum(1 for t, _ in self.inbox if t == msg_type)

    def all_inbox(self):
        with self.lock:
            return list(self.inbox)
