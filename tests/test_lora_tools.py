"""Offline provisioning/observer helper tests. No serial/network access."""
import asyncio
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


provision = load("provision-sniffer")
observer = load("lora-client")
dashboard = load("provision-dashboard")
usb = load("sniffer-serial")


class ProvisionTests(unittest.TestCase):
    def setUp(self):
        self.valid = {"type": "provision", "ssid": "LAN", "password": "private", "token": "0" * 64}

    def test_wire(self):
        wire = provision.validate(self.valid)
        self.assertEqual(json.loads(wire), self.valid)
        self.assertLessEqual(len(wire), 511)

    def test_rejection(self):
        for key, value in [("ssid", ""), ("ssid", "é" * 17), ("password", "x" * 65),
                           ("token", "short"), ("token", "x" * 96), ("token", "x" * 32 + "\r\n"),
                           ("ssid", "abc\0def"), ("type", "set_config"), ("extra", True)]:
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                provision.validate(dict(self.valid, **{key: value}))

    def test_private_files(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "credentials.json"
            wire = provision.validate(self.valid)
            provision.write_private(path, wire)
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            self.assertEqual(provision.read_private(path), wire)
            with self.assertRaises(FileExistsError):
                provision.write_private(path, b"overwrite")
            path.chmod(0o644)
            with self.assertRaises(ValueError):
                provision.read_private(path)
            link = Path(directory) / "link"
            link.symlink_to(path)
            with self.assertRaises(OSError):
                provision.read_private(link)


class Frames:
    def __init__(self, frames):
        self.frames = iter(frames)

    def __aiter__(self):
        return self

    async def __anext__(self):
        try:
            return next(self.frames)
        except StopIteration:
            raise StopAsyncIteration


class ObserverTests(unittest.TestCase):
    def test_summary_omits_payload(self):
        from types import SimpleNamespace
        frame = {"schema": 1, "type": "packet", "sequence": 1, "length": 3,
                 "rssi_dbm": -100, "snr_db": 4, "payload_hex": "SECRET"}
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            asyncio.run(observer.receive(Frames([json.dumps(frame)]), SimpleNamespace(raw=False)))
        self.assertIn("RX #1", out.getvalue())
        self.assertNotIn("SECRET", out.getvalue())

    def test_errors(self):
        from types import SimpleNamespace
        for raw in [b"binary", "[]", '{"schema":2}', '{"schema":1,"type":"error","message":"Authentication failed"}']:
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                asyncio.run(observer.receive(Frames([raw]), SimpleNamespace(raw=False)))


class SerialDiagnosticsTests(unittest.TestCase):
    def test_absent_ambiguous_and_unique_usb_devices(self):
        from types import SimpleNamespace
        device = SimpleNamespace(device="XIAO", vid=0x303A, pid=0x1001)
        self.assertEqual(usb.choose_port([device]), "XIAO")
        with self.assertRaisesRegex(ValueError, "No XIAO"):
            usb.choose_port([])
        with self.assertRaisesRegex(ValueError, "--port"):
            usb.choose_port([device, device])

    def test_control_lines_are_changed_atomically(self):
        import fcntl
        import struct
        import termios
        mask = struct.pack("I", termios.TIOCM_DTR | termios.TIOCM_RTS)
        with mock.patch.object(fcntl, "ioctl") as ioctl:
            usb.update_control_lines(42, False, False)
            ioctl.assert_called_once_with(42, termios.TIOCMBIC, mask)
            ioctl.reset_mock()
            usb.update_control_lines(42, True, True)
            ioctl.assert_called_once_with(42, termios.TIOCMBIS, mask)
            ioctl.reset_mock()
            with self.assertRaises(ValueError):
                usb.update_control_lines(42, False, True)
            ioctl.assert_not_called()

    def test_opener_disables_hangup_and_both_private_pyserial_hooks(self):
        import termios
        from types import SimpleNamespace
        class StubSerial:
            fd = 42
            _dtr_state = _rts_state = False
            def _reconfigure_port(self, *args, **kwargs):
                pass
        port = usb.diagnostic_port(SimpleNamespace(Serial=StubSerial), "XIAO")
        with mock.patch.object(usb, "update_control_lines") as update:
            port._update_dtr_state()
            port._update_rts_state()
            self.assertEqual(update.call_args_list, [mock.call(42, False, False)] * 2)
        attributes = [0, 0, termios.HUPCL | termios.CLOCAL, 0, 0, 0, []]
        with mock.patch.object(termios, "tcgetattr", return_value=attributes), mock.patch.object(termios, "tcsetattr") as setattrs:
            port._reconfigure_port()
            self.assertFalse(setattrs.call_args.args[2][2] & termios.HUPCL)
            self.assertTrue(setattrs.call_args.args[2][2] & termios.CLOCAL)

    def test_log_noise_fragments_and_resync(self):
        decoder = usb.DiagnosticLines()
        self.assertEqual(decoder.feed(b'boot\n{"schema":1,"type":"usb_'), [])
        frames = decoder.feed(b'wifi","state":"connecting"}\r\n')
        self.assertEqual(frames[0]["state"], "connecting")
        self.assertEqual(decoder.feed(b'x' * 5000), [])
        self.assertLessEqual(len(decoder.buffer), 4096)
        self.assertEqual(decoder.feed(b'\n[1,2]\n{"schema":true,"type":"usb_wifi"}\n'), [])
        self.assertEqual(len(decoder.feed(b'{"schema":1,"type":"usb_wifi"}\n')), 1)

    def test_summary_is_allowlisted_and_terminal_safe(self):
        summary = usb.format_wifi({"state": "connected", "ssid": "evil\x1b[2J\nname", "rssi_dbm": -82,
                                   "password": "DO_NOT_PRINT", "token": "DO_NOT_PRINT",
                                   "recent_disconnects": [{"reason": 204, "name": "HANDSHAKE_TIMEOUT"}]})
        self.assertIn("-82 dBm", summary)
        self.assertIn("HANDSHAKE_TIMEOUT", summary)
        self.assertNotIn("DO_NOT_PRINT", summary)
        self.assertNotIn("\x1b", summary)
        self.assertIn("does not prove LAN reachability", summary)
        self.assertIn("channel unavailable", summary)

    def test_read_only_poll(self):
        port = FakePort()
        port.read = lambda size: b'{"schema":1,"type":"usb_wifi","state":"disconnected"}\n'
        with contextlib.redirect_stdout(io.StringIO()) as out:
            usb.run(port)
        self.assertEqual(port.writes, [b"status\n"])
        self.assertIn("disconnected", out.getvalue())

    def test_missing_firmware_times_out(self):
        port = FakePort()
        port.read = lambda size: b"old firmware\n"
        with mock.patch.object(usb.time, "monotonic", side_effect=[0, 0, 9]):
            with self.assertRaises(TimeoutError):
                usb.run(port)
        self.assertEqual(port.writes, [b"status\n"])


class FakePort:
    def __init__(self, ready=True, saved=True):
        self.ready, self.saved, self.pending, self.writes = ready, saved, b"", []

    def reset_input_buffer(self):
        self.pending = b""

    def write(self, data):
        self.writes.append(data)
        if b"sniffer_setup_probe" in data:
            self.pending = b"DASH_USB_LINK_READY\n" if self.ready else b"wrong device\n"
        else:
            self.pending = b"DASH_USB_LINK_SAVED\n" if self.saved else b"DASH_USB_LINK_FAILED\n"

    def read(self, size):
        result, self.pending = self.pending[:size], self.pending[size:]
        return result

    def flush(self):
        pass


class DashboardSetupTests(unittest.TestCase):
    def test_scoped_wire_and_handshake(self):
        token = "0" * 64
        wire = dashboard.link_request(token)
        data = json.loads(wire)
        self.assertEqual(set(data), {"schema", "type", "host", "port", "token", "enabled"})
        self.assertEqual(data["type"], "provision_link")
        self.assertLessEqual(len(wire), 512)
        port = FakePort()
        with contextlib.redirect_stdout(io.StringIO()) as out:
            dashboard.send_request(port, wire)
        self.assertNotIn(token, out.getvalue())
        self.assertNotIn(token.encode(), port.writes[0])
        self.assertEqual(port.writes[1], wire)

    def test_invalid_input_and_save_failure(self):
        for token, host, port in [("short", "host", 8080), ("0" * 64, "ws://host/lora", 8080),
                                  ("0" * 64, "host", 0), ("0" * 64 + "\n", "host", 8080)]:
            with self.assertRaises(ValueError):
                dashboard.link_request(token, host, port)
        wire = dashboard.link_request("0" * 64)
        with self.assertRaises(ValueError):
            dashboard.send_request(FakePort(saved=False), wire)

    def test_wrong_device_never_gets_token(self):
        token = "0" * 64
        port = FakePort(ready=False)
        with mock.patch.object(dashboard.time, "monotonic", side_effect=[0, 0, 0, 0, 9]):
            with self.assertRaises(TimeoutError):
                dashboard.send_request(port, dashboard.link_request(token))
        self.assertTrue(port.writes)
        self.assertTrue(all(token.encode() not in data for data in port.writes))


if __name__ == "__main__":
    unittest.main()
