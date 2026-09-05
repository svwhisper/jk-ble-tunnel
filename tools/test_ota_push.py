"""Native, offline updater regressions: python3 -B -m unittest discover -s tools."""
import contextlib
import io
import os
import struct
import tempfile
import unittest
from unittest.mock import patch

import ota_push as ota


class OtaTests(unittest.TestCase):
    SHA = "12" * 32

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="jk-ota-test-")
        self.addCleanup(self.tmp.cleanup)
        self.path = os.path.join(self.tmp.name, "node_b.bin")
        self.header = bytearray(288)
        self.header[0] = 0xE9
        struct.pack_into("<H", self.header, 12, 5)
        struct.pack_into("<I", self.header, 28, 256)
        struct.pack_into("<I", self.header, 32, 0xABCD5432)
        self.header[80:86] = b"node_b"
        self.header[176:208] = bytes.fromhex(self.SHA)
        self.save()

    def save(self):
        with open(self.path, "wb") as f:
            f.write(self.header)

    def test_exact_descriptor_identity(self):
        self.assertEqual(ota.image_identity(self.path, "b"), self.SHA)

    def test_wrong_chip(self):
        with self.assertRaisesRegex(ValueError, "wrong chip"):
            ota.image_identity(self.path, "a")

    def test_wrong_project(self):
        self.header[80:86] = b"node_a"
        self.save()
        with self.assertRaisesRegex(ValueError, "wrong project"):
            ota.image_identity(self.path, "b")

    def test_short_or_corrupt_header(self):
        for n in (0, 1, 24, 32, 287):
            with self.subTest(n=n):
                with open(self.path, "wb") as f:
                    f.write(self.header[:n])
                with self.assertRaises(ValueError):
                    ota.image_identity(self.path, "b")
        self.header[32] = 0
        self.save()
        with self.assertRaisesRegex(ValueError, "descriptor"):
            ota.image_identity(self.path, "b")

    def test_zero_elf_hash(self):
        self.header[176:208] = bytes(32)
        self.save()
        with self.assertRaisesRegex(ValueError, "ELF identity"):
            ota.image_identity(self.path, "b")

    def test_status_reader_rejects_malformed_or_oversized_data(self):
        for payload in (b"not json", b"[]", b"x" * 1025):
            with self.subTest(payload=payload[:20]), patch.object(ota.urllib.request, "build_opener") as build:
                response = build.return_value.open.return_value.__enter__.return_value
                response.read.return_value = payload
                self.assertIsNone(ota.read_status("node", 3765))

    def test_status_reader_handles_network_failure(self):
        with patch.object(ota.urllib.request, "build_opener") as build:
            build.return_value.open.side_effect = OSError("timeout")
            self.assertIsNone(ota.read_status("node", 3765))

    def test_status_reader_accepts_bounded_json(self):
        with patch.object(ota.urllib.request, "build_opener") as build:
            response = build.return_value.open.return_value.__enter__.return_value
            response.read.return_value = b'{"ota_state":2}'
            self.assertEqual(ota.read_status("node", 3765), {"ota_state": 2})
            response.read.assert_called_once_with(1025)

    def test_only_exact_valid_image_is_confirmed(self):
        for status in (None, [], {}, {"elf_sha256": self.SHA},
                       {"elf_sha256": "34" * 32, "ota_state": 2}):
            self.assertFalse(ota.image_confirmed(status, self.SHA))
        for state in (-1, 0, 1, 3, 4, "2", True, None):
            self.assertFalse(ota.image_confirmed({"elf_sha256": self.SHA, "ota_state": state}, self.SHA))
        self.assertTrue(ota.image_confirmed({"elf_sha256": self.SHA, "ota_state": 2}, self.SHA))

    def test_wait_through_old_image_disconnect_and_pending(self):
        good = {"elf_sha256": self.SHA, "ota_state": 2}
        with patch.object(ota, "read_status", side_effect=[
                {"elf_sha256": "34" * 32, "ota_state": 2}, None,
                {"elf_sha256": self.SHA, "ota_state": 1}, good]), patch.object(ota.time, "sleep"):
            self.assertEqual(ota.wait_verified("node", 3765, self.SHA), good)

    def test_rollback_is_not_success(self):
        with patch.object(ota, "read_status", return_value={"elf_sha256": "34" * 32, "ota_state": 2}), \
                patch.object(ota.time, "monotonic", side_effect=[0, 0, 1, 3]), \
                patch.object(ota.time, "sleep"):
            with self.assertRaises(TimeoutError):
                ota.wait_verified("node", 3765, self.SHA, timeout=2)

    def invoke(self, before, rc=0, flags=()):
        argv = ["ota_push.py", "b", "--host", "192.168.3.234", "--bin", self.path, *flags]
        output = io.StringIO()
        with patch.object(ota.sys, "argv", argv), patch.object(ota, "port_open", return_value=True), \
                patch.object(ota, "read_status", return_value=before), \
                patch.object(ota.subprocess, "call", return_value=rc) as upload, \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            try:
                ota.main()
                code = 0
            except SystemExit as exc:
                code = exc.code
        return code, upload.call_count, output.getvalue()

    def test_already_running_does_not_flash(self):
        code, uploads, _ = self.invoke({"elf_sha256": self.SHA, "ota_state": 2})
        self.assertEqual((code, uploads), (0, 0))

    def test_legacy_receiver_requires_explicit_flag(self):
        code, uploads, _ = self.invoke(None)
        self.assertEqual((code, uploads), (1, 0))

    def test_pending_receiver_not_overwritten(self):
        code, uploads, _ = self.invoke({"elf_sha256": "34" * 32, "ota_state": 1})
        self.assertEqual((code, uploads), (1, 0))

    def test_failed_upload_reports_uncertain_not_unchanged(self):
        code, uploads, output = self.invoke(None, rc=28, flags=["--allow-legacy"])
        self.assertEqual((code, uploads), (1, 1))
        self.assertIn("outcome is uncertain", output)
        self.assertNotIn("running image unchanged", output)

    def test_no_wait_never_claims_verified(self):
        code, uploads, output = self.invoke(None, flags=["--allow-legacy", "--no-wait"])
        self.assertEqual((code, uploads), (0, 1))
        self.assertIn("NOT VERIFIED", output)


if __name__ == "__main__":
    unittest.main()
