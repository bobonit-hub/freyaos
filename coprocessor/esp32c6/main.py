import errno
import select
import socket
import struct
import time
import network
import esp32
import freya_link
from protocol import SIZE, encode, decode

FETCH, WIFI_ON, WIFI_OFF, CREDENTIALS, CONNECT, DISCONNECT, STATUS, \
    SCAN_START, SCAN_NEXT, PING_START, PING_RESULT, SOCKET, CLOSE, \
    SOCK_CONNECT, BIND, LISTEN, ACCEPT, SEND, RECV, SENDTO, RECVFROM, \
    TLS_CONNECT = range(22)
EVENT = 0x8000

AGAIN = -8
ARG = -3
IO = -7
NACK = -5
MAX_SOCKETS = 4

wlan = network.WLAN(network.STA_IF)
nvs = esp32.NVS("freya")
sockets = [None] * MAX_SOCKETS
tls_sockets = [False] * MAX_SOCKETS
scan = None
scan_pos = 0
scan_pending = False
ping_job = None
ping_pending = None
cache_seq = None
cache_response = None
empty = bytes(SIZE)
event = None
event_signaled = False
last_wlan_status = None


def ipv4(text):
    p = [int(x) for x in text.split(".")]
    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]


def iptext(value):
    return "%d.%d.%d.%d" % (value >> 24, (value >> 16) & 255,
                             (value >> 8) & 255, value & 255)


def credentials():
    try:
        sb, pb = bytearray(32), bytearray(64)
        sn = nvs.get_blob("ssid", sb)
        pn = nvs.get_blob("pass", pb)
        return bytes(sb[:sn]).decode(), bytes(pb[:pn]).decode()
    except OSError:
        return None, None


def status_payload():
    if not wlan.active():
        state = 0
    elif wlan.isconnected():
        state = 3
    elif wlan.status() == network.STAT_CONNECTING:
        state = 2
    elif wlan.status() < 0:
        state = 4
    else:
        state = 1
    cfg = wlan.ifconfig() if wlan.isconnected() else ("0.0.0.0",) * 4
    ssid = credentials()[0] or ""
    try:
        rssi = wlan.status("rssi") if wlan.isconnected() else 0
    except OSError:
        rssi = 0
    return struct.pack("<iiIII33s3x", state, rssi, ipv4(cfg[0]),
                       ipv4(cfg[2]), ipv4(cfg[1]), ssid.encode()[:32])


def alloc_socket(kind):
    for i in range(MAX_SOCKETS):
        if sockets[i] is None and not tls_sockets[i]:
            s = socket.socket(socket.AF_INET,
                              socket.SOCK_STREAM if kind == 1 else socket.SOCK_DGRAM)
            s.setblocking(False)
            sockets[i] = s
            return i
    raise OSError(errno.ENFILE)


def close_socket(handle):
    if tls_sockets[handle]:
        freya_link.tls_close(handle)
        tls_sockets[handle] = False
    elif sockets[handle] is not None:
        sockets[handle].close()
        sockets[handle] = None


def endpoint(data, off=2):
    addr, port, _ = struct.unpack_from("<IHH", data, off)
    return iptext(addr), port


def nonblocking(call):
    try:
        return 0, call()
    except OSError as e:
        if e.args and e.args[0] in (errno.EAGAIN, errno.EWOULDBLOCK,
                                    errno.EINPROGRESS, errno.ETIMEDOUT):
            return AGAIN, None
        return IO, None


def poll_event():
    global last_wlan_status
    state = wlan.status() if wlan.active() else 0
    if last_wlan_status is None:
        last_wlan_status = state
    elif state != last_wlan_status:
        last_wlan_status = state
        return struct.pack("<BB", 1, state & 255)
    active = [(i, s) for i, s in enumerate(sockets) if s is not None]
    if active:
        readable, _, _ = select.select([s for _, s in active], [], [], 0)
        if readable:
            sock = readable[0]
            handle = next(i for i, value in active if value is sock)
            return struct.pack("<BB", 2, handle)
    return None


def dispatch(op, data):
    global scan, scan_pos, scan_pending, ping_job, ping_pending
    if op == WIFI_ON:
        wlan.active(True)
        return 0, b""
    if op == WIFI_OFF:
        for i in range(MAX_SOCKETS):
            close_socket(i)
        wlan.active(False)
        return 0, b""
    if op == CREDENTIALS:
        parts = data.split(b"\0")
        if len(parts) < 3 or not parts[0]:
            return ARG, b""
        nvs.set_blob("ssid", parts[0])
        nvs.set_blob("pass", parts[1])
        nvs.commit()
        return 0, b""
    if op == CONNECT:
        ssid, password = credentials()
        if not ssid:
            return ARG, b""
        wlan.active(True)
        wlan.connect(ssid, password)
        return 0, b""
    if op == DISCONNECT:
        wlan.disconnect()
        return 0, b""
    if op == STATUS:
        return 0, status_payload()
    if op == SCAN_START:
        if not wlan.active():
            wlan.active(True)
        scan = None
        scan_pos = 0
        scan_pending = True
        return 0, b""
    if op == SCAN_NEXT:
        if scan_pending:
            return AGAIN, b""
        if scan is None:
            return ARG, b""
        if scan_pos >= len(scan):
            scan = None
            return NACK, b""
        ssid, bssid, channel, rssi, auth, hidden = scan[scan_pos]
        scan_pos += 1
        return 0, struct.pack("<iBB2x33s3x", rssi, channel, auth, ssid[:32])
    if op == PING_START:
        timeout = struct.unpack_from("<I", data)[0]
        host = data[4:].split(b"\0", 1)[0].decode()
        ping_job = None
        ping_pending = (host, timeout)
        return 0, b""
    if op == PING_RESULT:
        if ping_pending is not None or ping_job is None:
            return AGAIN, b""
        addr, elapsed, replies, lost = ping_job
        ping_job = None
        return 0, struct.pack("<IIII", addr, elapsed, replies, lost)
    if op == SOCKET:
        domain, kind, protocol = struct.unpack("<HHH", data)
        if domain != 2 or kind not in (1, 2):
            return ARG, b""
        return alloc_socket(kind), b""

    handle = struct.unpack_from("<H", data)[0]
    if (handle < 0 or handle >= MAX_SOCKETS or
            (sockets[handle] is None and not tls_sockets[handle])):
        return ARG, b""
    if op == TLS_CONNECT:
        if len(data) < 6:
            return ARG, b""
        port = struct.unpack_from("<H", data, 2)[0]
        host = data[4:].split(b"\0", 1)[0].decode()
        if not host or not port:
            return ARG, b""
        if not tls_sockets[handle]:
            sockets[handle].close()
            sockets[handle] = None
            tls_sockets[handle] = True
        connected = freya_link.tls_connect(handle, host, port)
        return (0 if connected else AGAIN), b""
    s = sockets[handle]
    if op == CLOSE:
        close_socket(handle)
        return 0, b""
    if tls_sockets[handle]:
        if op == SEND:
            count = freya_link.tls_write(handle, data[2:])
            return (AGAIN if count is None else count), b""
        if op == RECV:
            length = struct.unpack_from("<H", data, 2)[0]
            body = freya_link.tls_read(handle, length)
            return (AGAIN, b"") if body is None else (len(body), body)
        return ARG, b""
    if op == SOCK_CONNECT:
        status, _ = nonblocking(lambda: s.connect(endpoint(data)))
        return status, b""
    if op == BIND:
        s.bind(endpoint(data))
        return 0, b""
    if op == LISTEN:
        s.listen(struct.unpack_from("<H", data, 2)[0])
        return 0, b""
    if op == ACCEPT:
        status, result = nonblocking(s.accept)
        if status:
            return status, b""
        client, peer = result
        client.setblocking(False)
        slot = next((i for i, value in enumerate(sockets) if value is None), -1)
        if slot < 0:
            client.close()
            return IO, b""
        sockets[slot] = client
        address = struct.pack("<IHH", ipv4(peer[0]), peer[1], 0)
        return slot, address
    if op in (SEND, SENDTO):
        off = 2
        peer = None
        if op == SENDTO:
            peer = endpoint(data, 2)
            off += 8
        status, count = nonblocking(lambda: s.sendto(data[off:], peer)
                                    if peer else s.send(data[off:]))
        return status if status else count, b""
    if op in (RECV, RECVFROM):
        length = struct.unpack_from("<H", data, 2)[0]
        status, result = nonblocking(lambda: s.recvfrom(length)
                                     if op == RECVFROM else s.recv(length))
        if status:
            return status, b""
        if op == RECVFROM:
            body, peer = result
            return len(body), struct.pack("<IHH", ipv4(peer[0]), peer[1], 0) + body
        return len(result), result
    return ARG, b""


def serve():
    global cache_seq, cache_response, event, event_signaled
    global scan, scan_pending, ping_job, ping_pending
    queued_reply = False
    freya_link.init()
    freya_link.queue(empty, False)  # receive slot; READY means reply/event only
    while True:
        raw = freya_link.poll()
        if raw is None:
            if not queued_reply and scan_pending:
                scan = wlan.scan()
                scan_pending = False
            elif not queued_reply and ping_pending is not None:
                host, timeout = ping_pending
                ping_job = freya_link.ping(host, timeout)
                ping_pending = None
            if not queued_reply and event is None:
                event = poll_event()
            if event is not None and not event_signaled:
                freya_link.signal()
                event_signaled = True
            time.sleep_ms(1)
            continue
        request = decode(raw)
        if request is None:
            freya_link.queue(empty, False)
            continue
        op, sequence, ignored, data = request
        if op == FETCH:
            if queued_reply:
                queued_reply = False
                freya_link.queue(empty, False)
            elif event is not None:
                freya_link.queue(encode(EVENT, sequence, 0, event), True)
                event = None
                event_signaled = False
                queued_reply = True
            else:
                freya_link.queue(empty, False)
            continue
        if event is not None:
            event_signaled = False
        if sequence == cache_seq:
            response = cache_response
        else:
            try:
                status, payload = dispatch(op, data)
            except (OSError, ValueError, IndexError):
                status, payload = IO, b""
            response = encode(op, sequence, status, payload)
            if status != AGAIN:
                cache_seq, cache_response = sequence, response
        freya_link.queue(response, True)
        queued_reply = True


serve()
