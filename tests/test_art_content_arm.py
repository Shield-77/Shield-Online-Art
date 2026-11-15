"""Exercise the actual ARM preview checks in Unicorn, without a 3DS emulator.

The fixtures are only test data; no sample result is inserted in the app.
Run with the bundled Python after installing unicorn in .test-deps.
"""
import os
import base64
import ctypes
import hashlib
import json
import random
import re
import struct
import subprocess
import sys
import unittest
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / ".test-deps"))
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM  # noqa: E402
from unicorn.arm_const import (  # noqa: E402
    UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1,
    UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP,
)

TOOL_DIR = Path(os.environ.get("ART_ARM_TOOLS", r"C:\devkitPro\devkitARM\bin"))
BUILD = ROOT / "build" / "tests"
CODE = 0x10000
STOP = 0x100000
DATA = 0x400000
DATA_END = 0x500000
MIME = 0x600000
OUT = 0x700000
STACK = 0x800000
AUTHOUT = 0x900000
SP = STACK + 0xF000
OK, TOO_LARGE, BAD_MIME, MISMATCH, INVALID, UNSUPPORTED, DIMENSIONS = range(7)
DELETED_KNOWN, DELETED, MATURE_KNOWN, MATURE = 1, 2, 4, 8


class PageItem(ctypes.LittleEndianStructure):
    _fields_ = [("id", ctypes.c_char * 64), ("title", ctypes.c_char * 128),
                ("artist", ctypes.c_char * 64), ("page_url", ctypes.c_char * 512),
                ("preview_url", ctypes.c_char * 1024),
                ("preview_width", ctypes.c_uint32), ("preview_height", ctypes.c_uint32)]


class Page(ctypes.LittleEndianStructure):
    _fields_ = [("count", ctypes.c_uint32), ("has_more", ctypes.c_uint32),
                ("has_next_offset", ctypes.c_uint32), ("next_offset", ctypes.c_uint32),
                ("next_cursor", ctypes.c_char * 256), ("items", PageItem * 8)]


def run_tool(tool, *args):
    executable = TOOL_DIR / ("arm-none-eabi-" + tool + ".exe")
    result = subprocess.run([str(executable), *map(str, args)],
                            cwd=ROOT, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stderr)
    return result.stdout


class ArmHelpers:
    def __init__(self):
        BUILD.mkdir(parents=True, exist_ok=True)
        elf, raw = BUILD / "art-content.elf", BUILD / "art-content.bin"
        required = ("art_check_preview", "art_content_allowed", "art_parse_page", "oauth_pkce_verifier",
                    "oauth_pkce_challenge", "oauth_pkce_state", "oauth_pkce_state_matches",
                    "oauth_pkce_clear", "oauth_pkce_build_url", "oauth_pkce_parse_callback",
                    "oauth_session_begin", "oauth_session_accept", "oauth_session_clear", "oauth_exchange_form", "oauth_response_write", "auth_qr_encode", "auth_qr_draw_top", "auth_lan_accept_line", "auth_listener_open", "auth_listener_poll", "auth_listener_close", "mock_net_reset", "mock_net_input", "mock_net_closes")
        run_tool("gcc", "-marm", "-march=armv6k", "-mfloat-abi=hard", "-mtp=soft",
                 "-Os", "-Wall", "-Wextra", "-Werror", "-DNDEBUG", "-fno-builtin", "-nostdlib",
                 "-ffunction-sections", "-fdata-sections", "-fno-unwind-tables",
                 "-fno-asynchronous-unwind-tables", "-Iinclude",
                 r"-IC:\devkitPro\portlibs\3ds\include", r"-IC:\devkitPro\libctru\include", r"-LC:\devkitPro\portlibs\3ds\lib",
                 "-Wl,-Ttext=0x10000,-e,art_check_preview,--build-id=none,--gc-sections",
                 *("-Wl,--undefined=" + name for name in required),
                 "source/art_content.c", "source/art_page.c", "source/oauth_pkce.c", "source/oauth_session.c", "source/oauth_exchange.c", "source/oauth_transport.c", "source/auth_qr.c", "source/auth_lan.c", "source/auth_listener.c", "third_party/qrcodegen/qrcodegen.c", "tests/arm_memory_stubs.c", "tests/arm_socket_stubs.c",
                 "-o", elf, "-lmbedcrypto", "-lgcc")
        run_tool("objcopy", "-O", "binary", elf, raw)
        symbols = run_tool("nm", "-n", "--defined-only", elf)
        self.symbols = {}
        for line in symbols.splitlines():
            match = re.match(r"^([0-9a-fA-F]+)\s+\w\s+((?:art|oauth|auth|mock)_\w+)$", line)
            if match:
                self.symbols[match.group(2)] = int(match.group(1), 16)
        assert set(required) <= self.symbols.keys()
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.uc.mem_map(CODE, 0x100000)
        self.uc.mem_write(CODE, raw.read_bytes())
        self.uc.mem_map(DATA, 0x100000)
        self.uc.mem_map(MIME, 0x1000)
        self.uc.mem_map(OUT, 0x10000)
        self.uc.mem_map(STACK, 0x10000)
        self.uc.mem_map(AUTHOUT, 0x1000)

    def call(self, name, *args):
        self.uc.reg_write(UC_ARM_REG_SP, SP)
        self.uc.reg_write(UC_ARM_REG_LR, STOP)
        for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            self.uc.reg_write(reg, value)
        if len(args) > 4:
            self.uc.mem_write(SP, struct.pack("<" + "I" * (len(args) - 4), *args[4:]))
        self.uc.emu_start(self.symbols[name], STOP, timeout=10_000_000, count=25_000_000)
        if self.uc.reg_read(UC_ARM_REG_PC) != STOP:
            raise AssertionError("ARM function did not return")
        return self.uc.reg_read(UC_ARM_REG_R0)

    def preview(self, data, mime, *, length=None, guard=False, data_address=None):
        if length is None:
            length = len(data)
        address = data_address if data_address is not None else (DATA_END - len(data) if guard else DATA)
        if data and data_address is None:
            self.uc.mem_write(address, bytes(data))
        encoded = mime.encode("ascii")
        self.uc.mem_write(MIME, encoded or b"\0")
        self.uc.mem_write(OUT, b"\xAA" * 12)
        status = self.call("art_check_preview", address, length, MIME, len(encoded), OUT)
        return status, struct.unpack("<III", self.uc.mem_read(OUT, 12))

    def pkce(self, function, data, length=None):
        self.uc.mem_write(DATA, data or b"\0")
        self.uc.mem_write(OUT, b"\xAA" * 144)
        result = self.call(function, DATA, len(data) if length is None else length, OUT)
        output = bytes(self.uc.mem_read(OUT, 144)).split(b"\0", 1)[0]
        return bool(result), output.decode("ascii")

    def auth_url(self, redirect, state, challenge, capacity=1024):
        self.uc.mem_write(DATA, redirect)
        self.uc.mem_write(MIME, state + b"\0")
        self.uc.mem_write(OUT, challenge + b"\0")
        self.uc.mem_write(AUTHOUT, b"\xAA" * 1024)
        result = self.call("oauth_pkce_build_url", DATA, len(redirect), MIME, OUT, AUTHOUT, capacity)
        output = bytes(self.uc.mem_read(AUTHOUT, 1024)).split(b"\0", 1)[0]
        return bool(result), output.decode("ascii")

    def callback(self, query, state, capacity=257):
        self.uc.mem_write(DATA, query or b"\0")
        self.uc.mem_write(MIME, state + b"\0")
        self.uc.mem_write(OUT, b"\xAA" * 300)
        result = self.call("oauth_pkce_parse_callback", DATA, len(query), MIME, OUT, capacity)
        output = bytes(self.uc.mem_read(OUT, 257)).split(b"\0", 1)[0]
        return result, output.decode("ascii")

    def page(self, document, filtering=True, *, length=None, guard=False, data_address=None):
        if length is None:
            length = len(document)
        address = data_address if data_address is not None else (DATA_END - len(document) if guard else DATA)
        if document and data_address is None:
            self.uc.mem_write(address, document)
        self.uc.mem_write(OUT, b"\xAA" * ctypes.sizeof(Page))
        result = self.call("art_parse_page", address, length, int(filtering), OUT)
        page = Page.from_buffer_copy(bytes(self.uc.mem_read(OUT, ctypes.sizeof(Page))))
        return result, page


class ContentChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.arm = ArmHelpers()
        cls.jpeg = (ROOT / "assets" / "callie.jpg").read_bytes()
        cls.png = (ROOT / "icon.png").read_bytes()

    def test_session_cancel_wrong_state_and_one_time_callback(self):
        arm = self.arm
        entropy = bytes(range(48))
        redirect = b"https://example.org/callback"
        arm.uc.mem_write(DATA, entropy)
        arm.uc.mem_write(MIME, redirect)
        arm.uc.mem_write(OUT, b"\xaa" * 400)
        result = arm.call("oauth_session_begin", OUT, DATA, len(entropy), MIME, len(redirect), AUTHOUT, 1024)
        self.assertEqual(result, 1)
        session = bytes(arm.uc.mem_read(OUT, 328))
        state = session[44:67].split(b"\0", 1)[0]
        self.assertEqual(len(state), 22)
        wrong = b"code=ABC&state=" + b"A" * 22
        arm.uc.mem_write(DATA, wrong)
        self.assertEqual(arm.call("oauth_session_accept", OUT, DATA, len(wrong)), 3)
        query = b"code=ABC&state=" + state
        arm.uc.mem_write(DATA, query)
        self.assertEqual(arm.call("oauth_session_accept", OUT, DATA, len(query)), 0)
        self.assertEqual(arm.call("oauth_session_accept", OUT, DATA, len(query)), 2)
        arm.call("oauth_session_clear", OUT)
        self.assertEqual(bytes(arm.uc.mem_read(OUT, 328)), bytes(328))
        arm.uc.mem_write(DATA, entropy)
        self.assertEqual(arm.call("oauth_session_begin", OUT, DATA, 47, MIME, len(redirect), AUTHOUT, 1024), 0)
        self.assertEqual(bytes(arm.uc.mem_read(OUT, 328)), bytes(328))

    def test_qr_for_phone_decodes_and_fits_top_screen(self):
        import zxingcpp
        from PIL import Image
        arm = self.arm
        redirect = b"https://example.org/callback"
        success, url = arm.auth_url(redirect, b"A" * 22, b"B" * 43)
        self.assertTrue(success)
        data = url.encode("ascii")
        arm.uc.mem_write(DATA, data)
        self.assertEqual(arm.call("auth_qr_encode", DATA, len(data), MIME, OUT), 1)
        qr = bytes(arm.uc.mem_read(OUT, 1487))
        size = qr[0]
        self.assertLessEqual((size + 8) * 2, 240)
        image = Image.new("L", ((size + 8) * 4, (size + 8) * 4), 255)
        pixels = image.load()
        for y in range(size):
            for x in range(size):
                i = y * size + x
                if (qr[1 + (i >> 3)] >> (i & 7)) & 1:
                    for dy in range(4):
                        for dx in range(4):
                            pixels[(x + 4) * 4 + dx, (y + 4) * 4 + dy] = 0
        decoded = zxingcpp.read_barcode(image)
        self.assertIsNotNone(decoded)
        self.assertEqual(decoded.text, url)
        framebuffer_size = 400 * 240 * 3
        arm.uc.mem_write(DATA, b"\x55" * framebuffer_size)
        self.assertEqual(arm.call("auth_qr_draw_top", DATA, framebuffer_size - 1, OUT), 0)
        self.assertEqual(arm.call("auth_qr_draw_top", DATA, framebuffer_size, OUT), 1)
        frame = bytes(arm.uc.mem_read(DATA, framebuffer_size))
        rendered = Image.new("L", (400, 240))
        rendered.putdata([frame[(x * 240 + 239 - y) * 3] for y in range(240) for x in range(400)])
        decoded = zxingcpp.read_barcode(rendered)
        self.assertIsNotNone(decoded)
        self.assertEqual(decoded.text, url)

    def test_lan_callback_request_validation(self):
        arm = self.arm
        entropy, redirect = bytes(range(48)), b"https://example.org/callback"
        arm.uc.mem_write(DATA, entropy)
        arm.uc.mem_write(MIME, redirect)
        self.assertEqual(arm.call("oauth_session_begin", OUT, DATA, 48, MIME, len(redirect), AUTHOUT, 1024), 1)
        state = bytes(arm.uc.mem_read(OUT + 44, 22))
        for line in (b"POST /callback?code=abc&state=" + state + b" HTTP/1.1\r\n",
                     b"GET /evil?code=abc&state=" + state + b" HTTP/1.1\r\n",
                     b"GET /callback?code=abc&state=" + b"X" * 22 + b" HTTP/1.1\r\n"):
            arm.uc.mem_write(DATA, line)
            self.assertNotEqual(arm.call("auth_lan_accept_line", OUT, DATA, len(line)), 0)
        line = b"GET /callback?code=abc&state=" + state + b" HTTP/1.1\r\n"
        arm.uc.mem_write(DATA, line)
        self.assertEqual(arm.call("auth_lan_accept_line", OUT, DATA, len(line)), 0)
        self.assertNotEqual(arm.call("auth_lan_accept_line", OUT, DATA, len(line)), 0)

    def test_exchange_form_encodes_code_and_uses_no_client_secret(self):
        arm = self.arm
        entropy = bytes(range(48))
        redirect = b"https://example.org/callback"
        arm.uc.mem_write(DATA, entropy)
        arm.uc.mem_write(MIME, redirect)
        self.assertEqual(arm.call("oauth_session_begin", OUT, DATA, 48, MIME, len(redirect), AUTHOUT, 1024), 1)
        session = bytes(arm.uc.mem_read(OUT, 328))
        verifier, state = session[:44].split(b"\0", 1)[0], session[44:67].split(b"\0", 1)[0]
        query = b"code=abc%2Bdef%26ghi&state=" + state
        arm.uc.mem_write(DATA, query)
        self.assertEqual(arm.call("oauth_session_accept", OUT, DATA, len(query)), 0)
        self.assertEqual(arm.call("oauth_exchange_form", OUT, MIME, len(redirect), AUTHOUT, 1024), 1)
        body = bytes(arm.uc.mem_read(AUTHOUT, 1024)).split(b"\0", 1)[0].decode()
        parsed = parse_qs(body)
        self.assertEqual(parsed["code"], ["abc+def&ghi"])
        self.assertEqual(parsed["code_verifier"], [verifier.decode()])
        self.assertEqual(parsed["redirect_uri"], [redirect.decode()])
        self.assertNotIn("client_secret", parsed)
        self.assertEqual(arm.call("oauth_exchange_form", OUT, MIME, len(redirect), AUTHOUT, 10), 0)
        self.assertEqual(bytes(arm.uc.mem_read(AUTHOUT, 1)), b"\0")

    def test_network_response_buffer_is_bounded(self):
        arm = self.arm
        arm.uc.mem_write(DATA, b"abcdef")
        arm.uc.mem_write(MIME, struct.pack("<IIIB3x", OUT, 8, 0, 0))
        self.assertEqual(arm.call("oauth_response_write", DATA, 2, 3, MIME), 6)
        self.assertEqual(bytes(arm.uc.mem_read(OUT, 7)), b"abcdef\0")
        self.assertEqual(arm.call("oauth_response_write", DATA, 2, 1, MIME), 0)
        self.assertEqual(bytes(arm.uc.mem_read(MIME + 12, 1)), b"\1")
        self.assertEqual(arm.call("oauth_response_write", DATA, 0x10000, 0x10000, MIME), 0)

    def test_listener_nonblocking_success_and_timeout(self):
        arm = self.arm
        entropy, redirect = bytes(range(48)), b"https://example.org/callback"
        arm.uc.mem_write(DATA, entropy)
        arm.uc.mem_write(MIME, redirect)
        self.assertEqual(arm.call("oauth_session_begin", OUT, DATA, 48, MIME, len(redirect), AUTHOUT, 1024), 1)
        state = bytes(arm.uc.mem_read(OUT + 44, 22))
        server = AUTHOUT
        arm.call("mock_net_reset")
        self.assertEqual(arm.call("auth_listener_open", server, 0x2a01a8c0), 1)
        line = b"GET /callback?code=abc&state=" + state + b" HTTP/1.1\r\n"
        arm.uc.mem_write(DATA, line)
        arm.call("mock_net_input", DATA, len(line))
        self.assertEqual(arm.call("auth_listener_poll", server, OUT, 100, 0), 0)
        arm.call("auth_listener_close", server)
        self.assertEqual(arm.call("mock_net_closes"), 2)
        arm.uc.mem_write(DATA, entropy)
        self.assertEqual(arm.call("oauth_session_begin", OUT, DATA, 48, MIME, len(redirect), AUTHOUT + 2048, 1024), 1)
        arm.call("mock_net_reset")
        self.assertEqual(arm.call("auth_listener_open", server, 0x2a01a8c0), 1)
        self.assertEqual(arm.call("auth_listener_poll", server, OUT, 100, 0), 2)
        self.assertEqual(arm.call("auth_listener_poll", server, OUT, 10200, 0), 2)
        self.assertEqual(arm.call("mock_net_closes"), 1)
        arm.call("auth_listener_close", server)
        self.assertEqual(arm.call("mock_net_closes"), 2)

    def test_mature_policy_and_unknown_fields(self):
        cases = [
            (1, DELETED_KNOWN | MATURE_KNOWN, True),
            (1, DELETED_KNOWN | MATURE_KNOWN | MATURE, False),
            (1, DELETED_KNOWN, False),
            (0, DELETED_KNOWN, True),
            (0, DELETED_KNOWN | MATURE_KNOWN | MATURE, True),
            (0, DELETED_KNOWN | DELETED | MATURE_KNOWN, False),
            (1, MATURE_KNOWN, False),
            (0, 0, False),
        ]
        for filtering, flags, expected in cases:
            with self.subTest(filtering=filtering, flags=flags):
                self.assertEqual(bool(self.arm.call("art_content_allowed", filtering, flags)), expected)

    def test_real_local_jpeg_and_png_headers(self):
        self.assertEqual(self.arm.preview(self.jpeg, "image/jpeg"), (OK, (640, 360, 1)))
        self.assertEqual(self.arm.preview(self.png, "image/png"), (OK, (48, 48, 2)))
        self.assertEqual(self.arm.preview(self.jpeg, "  Image/JPEG ; charset=binary"), (OK, (640, 360, 1)))

    def test_mime_spoofing_unknown_and_empty(self):
        self.assertEqual(self.arm.preview(self.png, "image/jpeg")[0], MISMATCH)
        self.assertEqual(self.arm.preview(self.jpeg, "image/png")[0], MISMATCH)
        self.assertEqual(self.arm.preview(self.jpeg, "image/jpegx")[0], BAD_MIME)
        self.assertEqual(self.arm.preview(self.png, "image/webp")[0], BAD_MIME)
        self.assertEqual(self.arm.preview(self.png, "image/png;" + "x" * 250)[0], BAD_MIME)
        self.assertEqual(self.arm.preview(b"", "image/png")[0], INVALID)

    def test_png_bounds_interlace_and_corrupt_ihdr(self):
        for dimensions in [(0, 48), (2049, 1), (2048, 2048)]:
            image = bytearray(self.png)
            image[16:24] = struct.pack(">II", *dimensions)
            self.assertEqual(self.arm.preview(image, "image/png")[0], DIMENSIONS)
        image = bytearray(self.png)
        image[28] = 1
        self.assertEqual(self.arm.preview(image, "image/png")[0], UNSUPPORTED)
        image[28] = 0
        image[12:16] = b"FAIL"
        self.assertEqual(self.arm.preview(image, "image/png")[0], INVALID)
        for length in range(1, 33):
            result, _ = self.arm.preview(self.png[:length], "image/png", guard=True)
            self.assertIn(result, (MISMATCH, INVALID))

    def test_jpeg_truncation_and_oversize_before_read(self):
        malformed = bytes.fromhex("ffd8 ffe0 ffff 0102")
        self.assertEqual(self.arm.preview(malformed, "image/jpeg", guard=True)[0], INVALID)
        marker_bomb = b"\xff\xd8" + b"\xff\x01" * 1030
        self.assertEqual(self.arm.preview(marker_bomb, "image/jpeg", guard=True)[0], INVALID)
        result, info = self.arm.preview(b"", "image/jpeg", length=2 * 1024 * 1024 + 1,
                                        data_address=0x2000000)
        self.assertEqual((result, info), (TOO_LARGE, (0, 0, 0)))

    def test_short_random_headers_never_read_past_end(self):
        rng = random.Random(81410)
        for _ in range(400):
            size = rng.randrange(1, 256)
            payload = bytearray(rng.getrandbits(8) for _ in range(size))
            mime = "image/jpeg" if rng.randrange(2) else "image/png"
            if mime == "image/jpeg" and size >= 3:
                payload[:3] = b"\xff\xd8\xff"
            elif mime == "image/png" and size >= 8:
                payload[:8] = b"\x89PNG\r\n\x1a\n"
            status, _ = self.arm.preview(payload, mime, guard=True)
            self.assertIn(status, range(7))

    def test_pkce_rfc7636_s256_vector_and_rejection(self):
        verifier = b"dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"
        self.assertEqual(self.arm.pkce("oauth_pkce_challenge", verifier),
                         (True, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"))
        self.assertEqual(self.arm.pkce("oauth_pkce_challenge", b"a" * 42), (False, ""))
        self.assertEqual(self.arm.pkce("oauth_pkce_challenge", b"a" * 129), (False, ""))
        self.assertEqual(self.arm.pkce("oauth_pkce_challenge", b"a" * 42 + b"!"), (False, ""))

    def test_pkce_entropy_encoding_state_and_clearing(self):
        random_bytes = bytes(range(32))
        verifier = base64.urlsafe_b64encode(random_bytes).decode().rstrip("=")
        self.assertEqual(self.arm.pkce("oauth_pkce_verifier", random_bytes), (True, verifier))
        expected = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).decode().rstrip("=")
        self.assertEqual(self.arm.pkce("oauth_pkce_challenge", verifier.encode()), (True, expected))
        self.assertEqual(self.arm.pkce("oauth_pkce_verifier", random_bytes[:31]), (False, ""))
        state = base64.urlsafe_b64encode(random_bytes[:16]).decode().rstrip("=")
        self.assertEqual(self.arm.pkce("oauth_pkce_state", random_bytes[:16]), (True, state))
        self.arm.uc.mem_write(DATA, state.encode())
        self.arm.uc.mem_write(MIME, state.encode())
        self.assertEqual(self.arm.call("oauth_pkce_state_matches", DATA, 22, MIME, 22), 1)
        self.arm.uc.mem_write(MIME, (state[:-1] + ("A" if state[-1] != "A" else "B")).encode())
        self.assertEqual(self.arm.call("oauth_pkce_state_matches", DATA, 22, MIME, 22), 0)
        self.assertEqual(self.arm.call("oauth_pkce_state_matches", DATA, 22, MIME, 21), 0)
        self.arm.uc.mem_write(DATA, random_bytes)
        self.arm.call("oauth_pkce_clear", DATA, len(random_bytes))
        self.assertEqual(bytes(self.arm.uc.mem_read(DATA, len(random_bytes))), bytes(len(random_bytes)))

    def test_auth_url_uses_public_client_and_s256(self):
        state = b"ABCDEFGHIJKLMNOPQRSTUV"
        challenge = b"E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"
        redirect = b"https://client.example/3ds/callback"
        success, url = self.arm.auth_url(redirect, state, challenge)
        self.assertTrue(success)
        parsed = urlsplit(url)
        self.assertEqual((parsed.scheme, parsed.netloc, parsed.path),
                         ("https", "www.deviantart.com", "/oauth2/authorize"))
        self.assertEqual(parse_qs(parsed.query), {
            "response_type": ["code"], "client_id": ["81410"],
            "redirect_uri": [redirect.decode()], "scope": ["browse"],
            "state": [state.decode()], "code_challenge": [challenge.decode()],
            "code_challenge_method": ["S256"],
        })
        for bad in (b"http://client.example/callback", b"https://evil@client.example/x",
                    b"https://client.example/x#frag", b"https://client.example/x?y=1",
                    b"https://client.example/%00", b"https:///missing-host",
                    b"https://-bad.example/x", b"https://bad..example/x",
                    b"https://bad-.example/x", b"https://trailing.example./x"):
            self.assertEqual(self.arm.auth_url(bad, state, challenge), (False, ""))
        self.assertEqual(self.arm.auth_url(redirect, state, challenge, capacity=8), (False, ""))
        self.assertEqual(self.arm.auth_url(redirect, state, challenge[:-1] + b"!"), (False, ""))

    def test_callback_checks_state_duplicates_and_encoded_code(self):
        state = b"ABCDEFGHIJKLMNOPQRSTUV"
        self.assertEqual(self.arm.callback(b"code=ABC%2Bdef%3D&state=" + state, state), (0, "ABC+def="))
        self.assertEqual(self.arm.callback(b"state=" + state + b"&error=access_denied", state), (1, ""))
        self.assertEqual(self.arm.callback(b"code=secret&state=ABCDEFGHIJKLMNOPQRSTUW", state), (3, ""))
        for query in (b"code=foo&state=" + state + b"&state=" + state,
                      b"code=foo&code=bar&state=" + state,
                      b"code=foo&error=access_denied&state=" + state,
                      b"code=%0G&state=" + state, b"code=%00&state=" + state,
                      b"code=foo&state=" + state + b"&", b"code=foo"):
            with self.subTest(query=query):
                self.assertEqual(self.arm.callback(query, state), (2, ""))
        self.assertEqual(self.arm.callback(b"code=abc&state=" + state, state, capacity=3), (4, ""))

    @staticmethod
    def result_item(item_id="art-123", **changes):
        item = {
            "deviationid": item_id,
            "title": "Caf\u00e9 \U0001f3a8",
            "url": "https://www.deviantart.com/artist/art/works-123",
            "author": {"username": "Artist"},
            "is_deleted": False,
            "is_mature": False,
            "preview": {"src": "https://images-wixmp-a.wixmp.com/large.jpg", "width": 1280, "height": 720},
            "thumbs": [{"src": "https://images-wixmp-a.wixmp.com/tiny.jpg", "width": 120, "height": 80},
                       {"src": "https://images-wixmp-a.wixmp.com/medium.jpg?x=1&y=2", "width": 640, "height": 360}],
        }
        item.update(changes)
        return item

    def test_page_filter_attribution_thumbnail_and_pagination(self):
        safe = self.result_item()
        mature = self.result_item("mature-456", is_mature=True)
        unknown = self.result_item("unknown-789")
        del unknown["is_mature"]
        deleted = self.result_item("deleted-101", is_deleted=True)
        malformed_flag = self.result_item("bad-flag-1", is_mature="false")
        page_json = json.dumps({"results": [safe, mature, unknown, deleted, malformed_flag],
                                "has_more": True, "next_offset": 10,
                                "next_cursor": "next+cursor"}).encode()
        status, page = self.arm.page(page_json, filtering=True)
        self.assertEqual((status, page.count, page.has_more, page.has_next_offset, page.next_offset),
                         (0, 1, 1, 1, 10))
        self.assertEqual(page.next_cursor, b"next+cursor")
        self.assertEqual(page.items[0].id, b"art-123")
        self.assertEqual(page.items[0].title.decode(), "Caf\u00e9 \U0001f3a8")
        self.assertEqual(page.items[0].artist, b"Artist")
        self.assertEqual((page.items[0].preview_width, page.items[0].preview_height), (640, 360))
        self.assertIn(b"medium.jpg", page.items[0].preview_url)
        status, page = self.arm.page(page_json, filtering=False)
        self.assertEqual((status, page.count), (0, 4))

    def test_page_rejects_untrusted_urls_and_duplicate_security_fields(self):
        safe = self.result_item()
        fake_host = self.result_item("bad-host", url="https://www.deviantart.com.evil.example/a")
        missing_artist = self.result_item("bad-artist", author={})
        preview = {"src": "https://images.deviantart.net@evil.example/image.png", "width": 400, "height": 240}
        untrusted_preview = self.result_item("untrusted-preview", preview=preview, thumbs=[])
        data = json.dumps({"results": [safe, fake_host, missing_artist, untrusted_preview],
                           "has_more": False, "next_offset": None}).encode()
        status, page = self.arm.page(data, filtering=True)
        self.assertEqual((status, page.count), (0, 2))
        self.assertEqual(page.items[1].id, b"untrusted-preview")
        self.assertEqual(page.items[1].preview_url, b"")
        duplicate = (b'{"results":[{"deviationid":"x","url":"https://www.deviantart.com/a",'
                     b'"author":{"username":"a"},"is_deleted":false,"is_mature":false,'
                     b'"\\u0069s_mature":true}],"has_more":false}')
        self.assertEqual(self.arm.page(duplicate, filtering=True)[1].count, 0)
        self.assertEqual(self.arm.page(duplicate, filtering=False)[1].count, 0)

    def test_page_malformed_oversize_and_guarded_inputs(self):
        safe = json.dumps(self.result_item()).encode()
        documents = [
            b'{"results":[' + safe + b'],"has_more":false,}',
            b'{"results":[' + safe + b'],"has_more":false}junk',
            b'{"results":[' + safe + b'],"has_more":false,"has_more":true}',
            b'{"results":[],"has_more":"false"}',
            b'{"results":[],"has_more":false,"x":' + b'[' * 20 + b'0' + b']' * 20 + b'}',
            b'{"results":[],"has_more":false,"x":"\\uD800"}',
            b'{"results":[],"has_more":false,"x":"\xff"}',
        ]
        for document in documents:
            with self.subTest(document=document[:35]):
                result, page = self.arm.page(document, guard=True)
                self.assertEqual((result, page.count), (1, 0))
        result, page = self.arm.page(b"", length=65537, data_address=0x2000000)
        self.assertEqual((result, page.count), (2, 0))
        rng = random.Random(81411)
        for _ in range(200):
            garbage = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 160)))
            result, _ = self.arm.page(b'{"results":[' + garbage + b'],"has_more":false}', guard=True)
            self.assertIn(result, (0, 1))


if __name__ == "__main__":
    unittest.main(verbosity=2)
