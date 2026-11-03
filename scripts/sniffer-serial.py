#!/usr/bin/env python3
"""Read XIAO USB connection diagnostics without Wi-Fi or an access token.

Examples: sniffer-serial.py status; sniffer-serial.py watch
Use --port PATH if multiple Espressif USB devices are attached. Linux-only
no-reset opening: changes DTR/RTS together, never pulses either reset line.
Does not provision, scan Wi-Fi, or change the radio configuration.
"""
import argparse
import json
import time


class DiagnosticLines:
    """Bound memory and ignore boot logs, partial lines and unrelated firmware."""
    def __init__(self):
        self.buffer = bytearray()
        self.discard = False

    def feed(self, chunk):
        messages = []
        for byte in chunk:
            if byte == 10:
                if not self.discard:
                    try:
                        message = json.loads(self.buffer)
                    except (ValueError, UnicodeError):
                        message = None
                    if isinstance(message, dict) and type(message.get("schema")) is int and message["schema"] == 1:
                        if message.get("type") in ("usb_wifi", "usb_help", "usb_error", "usb_watch", "status"):
                            messages.append(message)
                self.buffer.clear()
                self.discard = False
            elif not self.discard:
                if len(self.buffer) >= 4096:
                    self.buffer.clear()
                    self.discard = True
                else:
                    self.buffer.append(byte)
        return messages


def safe(value):
    return "".join(c if c.isprintable() else "?" for c in str(value))[:160]


def format_wifi(data):
    rssi = data.get("rssi_dbm")
    signal = "unavailable" if rssi is None else f"{safe(rssi)} dBm"
    channel = "unavailable" if data.get("channel") is None else safe(data["channel"])
    lines = [f"Wi-Fi: {safe(data.get('state', 'unknown'))} | signal {signal} | channel {channel}",
             f"  Uptime: {safe(data.get('uptime_ms', 0))} ms | boot: {safe(data.get('boot_id', ''))} | reset reason: {safe(data.get('reset_reason', 0))}",
             f"  SSID: {safe(data.get('ssid', ''))} | AP: {safe(data.get('bssid', ''))} | MAC: {safe(data.get('mac', ''))}",
             f"  IP: {safe(data.get('ip', '')) or '-'} | gateway: {safe(data.get('gateway', '')) or '-'} | DNS: {safe(data.get('dns', '')) or '-'}",
             f"  Attempts: {safe(data.get('attempts', 0))} | disconnects: {safe(data.get('disconnects', 0))} | attempt timeouts: {safe(data.get('attempt_timeouts', 0))}",
             f"  Retry in: {safe(data.get('retry_in_ms', 0))} ms | last connect error: {safe(data.get('last_connect_error', 0))} | power-save mode: {safe(data.get('power_save', -1))}",
             f"  WebSocket listener ready: {safe(data.get('server_ready', False))} | open authenticated sockets: {safe(data.get('ws_clients', 0))}",
             f"  Authenticated sessions: {safe(data.get('ws_authenticated', 0))} | auth rejections: {safe(data.get('ws_auth_rejected', 0))}"]
    lines.append(f"  Free heap: {safe(data.get('free_heap', 0))} bytes | minimum console stack headroom: {safe(data.get('console_stack_min_free', 0))} bytes")
    lines.append(f"  Minimum Wi-Fi event-task stack headroom: {safe(data.get('event_stack_min_free', 0))} bytes")
    history = data.get("recent_disconnects")
    if isinstance(history, list) and history and isinstance(history[0], dict):
        last = history[0]
        lines.append(f"  Last disconnect: {safe(last.get('reason'))} {safe(last.get('name'))} | RSSI {safe(last.get('rssi_dbm'))} dBm | uptime {safe(last.get('at_ms'))} ms")
    lines.append("  Listener/IP status does not prove LAN reachability or dashboard authentication.")
    return "\n".join(lines)


def choose_port(ports):
    candidates = [p.device for p in ports if p.vid == 0x303A and p.pid == 0x1001]
    if not candidates:
        raise ValueError("No XIAO Espressif USB serial device detected; check USB power/cable and reconnect it")
    if len(candidates) > 1:
        raise ValueError("Use --port with the XIAO USB serial path (could not identify one unique Espressif device)")
    return candidates[0]


def update_control_lines(fd, dtr, rts):
    # ESP32-S3 native USB interprets asymmetric RTS/DTR states as reset/BOOT.
    # pyserial normally changes them with two ioctls, momentarily resetting the
    # device even when both desired final values are False. One combined ioctl
    # avoids that intermediate state and preserves all unrelated modem bits.
    import fcntl
    import struct
    import termios
    if dtr != rts:
        raise ValueError("Diagnostic serial must not drive asymmetric reset/BOOT lines")
    mask = struct.pack("I", termios.TIOCM_DTR | termios.TIOCM_RTS)
    fcntl.ioctl(fd, termios.TIOCMBIS if dtr else termios.TIOCMBIC, mask)


def diagnostic_port(serial_module, path):
    import sys
    if not sys.platform.startswith("linux"):
        raise ValueError("The no-reset diagnostic serial opener currently supports Linux only")
    import termios

    class DiagnosticSerial(serial_module.Serial):
        def _update_dtr_state(self):
            update_control_lines(self.fd, self._dtr_state, self._rts_state)

        def _update_rts_state(self):
            update_control_lines(self.fd, self._dtr_state, self._rts_state)

        def _reconfigure_port(self, *args, **kwargs):
            super()._reconfigure_port(*args, **kwargs)
            # Do not drop modem lines when the last reader closes the port.
            attributes = termios.tcgetattr(self.fd)
            if attributes[2] & termios.HUPCL:
                attributes[2] &= ~termios.HUPCL
                termios.tcsetattr(self.fd, termios.TCSANOW, attributes)

    port = DiagnosticSerial()
    port.port, port.baudrate, port.timeout, port.write_timeout = path, 115200, .2, 2
    port.dtr = port.rts = False
    port.exclusive = True
    return port


def run(port, watch=False, raw=False, timeout=8):
    decoder = DiagnosticLines()
    deadline = time.monotonic() + timeout
    next_request = 0
    received = False
    port.reset_input_buffer()
    while watch or not received:
        now = time.monotonic()
        if now >= next_request:
            # Polling, rather than enabling firmware streaming, leaves no active
            # watch after this process exits or USB is unplugged.
            port.write(b"status\n")
            port.flush()
            next_request = now + (1 if received else 2)
        for message in decoder.feed(port.read(1024)):
            kind = message["type"]
            if kind == "usb_error":
                raise ValueError("XIAO rejected the diagnostics command; check the installed firmware")
            if kind == "usb_wifi":
                received = True
                deadline = time.monotonic() + timeout
                print(json.dumps(message, ensure_ascii=True) if raw else format_wifi(message), flush=True)
            elif kind == "status":
                if raw:
                    print(json.dumps(message, ensure_ascii=True), flush=True)
                else:
                    print(f"  Radio ready: {safe(message.get('radio_ready'))} | RX packets: {safe(message.get('rx_packets', 0))}", flush=True)
        if time.monotonic() >= deadline:
            raise TimeoutError("No XIAO USB diagnostics response. Check the port, close other serial monitors and install the diagnostics firmware.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("status", "watch"), nargs="?", default="status")
    parser.add_argument("--port")
    parser.add_argument("--json", action="store_true", help="Output structured snapshots; still excludes credentials")
    args = parser.parse_args()
    import serial
    from serial.tools import list_ports
    path = args.port or choose_port(list_ports.comports())
    port = diagnostic_port(serial, path)
    try:
        port.open()
        run(port, args.command == "watch", args.json)
    finally:
        port.close()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except (OSError, ValueError, TimeoutError) as error:
        raise SystemExit(str(error)) from None
