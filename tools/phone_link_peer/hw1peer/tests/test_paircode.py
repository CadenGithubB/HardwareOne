"""paircode: the X4 code vectors, the X3 passphrase vectors and the PSK.

X4 = phone_console_v2_records.json ``code_vectors`` (BLE_PhonePairCode.h and
scPbkdf2 on the host); X3 = phone_pipe_v1_passphrase.json (the app's entry rule
and the P4's ``phone secret`` path).
"""

import random

from hw1peer import paircode
from hw1peer.paircode import CodeFault, EntryFault, EntryRefused, PipeSecret, SecretKind
from hw1peer.tests import load_fixture, unhex

X4 = "phone_console_v2_records.json"
X3 = "phone_pipe_v1_passphrase.json"
GOLDEN = "phone_secure_channel_v1_pipe_v2_vectors.json"

_psk_memo: dict[bytes, str] = {}


def psk_hex(secret: bytes) -> str:
    """paircode.psk, memoised per secret within this test module (100k rounds each)."""
    if secret not in _psk_memo:
        _psk_memo[secret] = paircode.psk(secret).hex()
    return _psk_memo[secret]


# ---- the field and the check symbol ----------------------------------------------

def test_field_reference_values():
    # test_phone_pair_code.py's own reference: x * x^4 = x^5 = x^2 + 1, weight of v0 = alpha.
    assert paircode.gf_mul(2, 16) == 5
    assert paircode.check_value([1] + [0] * 14) == 2
    weights, w = [], 1
    for _ in range(31):
        w = paircode.gf_mul_x(w)
        weights.append(w)
    assert len(set(weights)) == 31 and 0 not in weights and w == 1, "alpha has order 31"


def test_gf_mul_matches_carryless_reference():
    def reference(a: int, b: int) -> int:
        product = 0
        for bit in range(5):
            if b >> bit & 1:
                product ^= a << bit
        for bit in range(8, 4, -1):
            if product >> bit & 1:
                product ^= 0b100101 << (bit - 5)
        return product

    for a in range(32):
        for b in range(32):
            assert paircode.gf_mul(a, b) == reference(a, b), (a, b)


def test_fixture_code_metadata():
    code = load_fixture(X4)["code"]
    assert code["alphabet"] == paircode.ALPHABET
    assert code["symbols"] == paircode.SYMBOLS


# ---- X4 code vectors -----------------------------------------------------------------

def test_x4_code_vectors_parse_and_reason():
    vectors = load_fixture(X4)["code_vectors"]
    assert len(vectors) == 535
    for v in vectors:
        entry = unhex(v["input_hex"])
        canonical = paircode.parse(entry)
        assert canonical == v["canonical"], v["id"]
        assert paircode.normalise(entry) == canonical
        assert paircode.is_code(entry) is v["accepted"], v["id"]
        fault = paircode.diagnose(entry)
        assert (fault.value if fault else "") == v["reason"], v["id"]
        # The same entry as text (every X4 input is UTF-8).
        assert paircode.parse(entry.decode("utf-8")) == canonical, v["id"]
        assert paircode.parse(bytearray(entry)) == canonical, v["id"]


def test_x4_code_vectors_display_and_normalised():
    for v in load_fixture(X4)["code_vectors"]:
        if not v["accepted"]:
            assert v["display"] is None and v["normalised_hex"] is None and v["psk_hex"] is None
            continue
        assert paircode.display(v["canonical"]) == v["display"], v["id"]
        assert len(v["display"]) == paircode.DISPLAY_LEN
        assert v["canonical"].encode("ascii").hex() == v["normalised_hex"], v["id"]
        # The display form parses back to the same code.
        assert paircode.parse(v["display"]) == v["canonical"]


def test_x4_code_vectors_psk():
    accepted = [v for v in load_fixture(X4)["code_vectors"] if v["accepted"]]
    assert len(accepted) == 14
    for v in accepted:
        assert psk_hex(v["canonical"].encode("ascii")) == v["psk_hex"], v["id"]
        secret = paircode.pipe_secret(unhex(v["input_hex"]))
        assert isinstance(secret, PipeSecret) and secret.kind is SecretKind.CODE and secret.is_code, v["id"]
        assert bytes(secret.secret) == v["canonical"].encode("ascii"), v["id"]


def test_x4_generation():
    by_id = {v["id"]: v["canonical"] for v in load_fixture(X4)["code_vectors"]}
    assert paircode.from_values([0] * 15) == by_id["canonical_zeros"]
    assert paircode.from_values([31] * 15) == by_id["canonical_all_31"]
    assert paircode.from_values(list(range(15))) == by_id["canonical_sequence"]
    assert paircode.from_values([17 + i for i in range(15)]) == by_id["canonical_tail"]
    assert paircode.from_random([0x01234567, 0x89ABCDEF, 0x0F1E2D3C]) == by_id["canonical_from_random_words"]
    # generate() takes 12 random bytes as three little-endian words.
    raw = (0x01234567).to_bytes(4, "little") + (0x89ABCDEF).to_bytes(4, "little") + (0x0F1E2D3C).to_bytes(4, "little")
    assert paircode.generate(lambda n: raw[:n]) == by_id["canonical_from_random_words"]


def test_x4_rejects_every_single_error_and_neighbour_swap():
    ids = {v["id"] for v in load_fixture(X4)["code_vectors"] if not v["accepted"]}
    assert sum(i.startswith("single_error_") for i in ids) == 16 * 31
    assert sum(i.startswith("swap_") for i in ids) == 15


def test_generated_codes_detect_errors_and_swaps():
    rng = random.Random(1234)
    for _ in range(200):
        code = paircode.generate(rng.randbytes)
        assert paircode.parse(code) == code and paircode.diagnose(code) is None
        for i in range(16):
            for symbol in paircode.ALPHABET:
                if symbol != code[i]:
                    assert paircode.diagnose(code[:i] + symbol + code[i + 1:]) is CodeFault.CHECK
            for j in range(i + 1, 16):
                if code[i] != code[j]:
                    swapped = list(code)
                    swapped[i], swapped[j] = swapped[j], swapped[i]
                    assert paircode.diagnose("".join(swapped)) is CodeFault.CHECK


def test_entry_normalisation_rules():
    code = "0123456789ABCDE4"
    assert paircode.parse("  0123-4567-89ab-cde4\r\n") == code
    assert paircode.parse("OI23 4567 89AB CDE4") == code  # O -> 0, I -> 1
    assert paircode.parse("ol23456789abcde4") == code  # o -> 0, l -> 1
    assert paircode.diagnose("0123U56789ABCDE4") is CodeFault.SYMBOL
    assert paircode.diagnose("0123456789ABCDE4_") is CodeFault.SYMBOL
    assert paircode.diagnose("0123456789ABCDEÉ") is CodeFault.SYMBOL
    assert paircode.diagnose("0123456789ABCDE\ud800") is CodeFault.SYMBOL  # lone surrogate
    assert paircode.diagnose("") is CodeFault.LENGTH
    assert paircode.diagnose("0123456789ABCDE40") is CodeFault.LENGTH


def test_display_needs_canonical():
    for bad in ("0123-4567-89AB-CDE4", "0123456789abcde4", "0123456789ABCDE", "0123456789ABCDEU"):
        try:
            paircode.display(bad)
        except ValueError:
            continue
        raise AssertionError(f"display accepted {bad!r}")


# ---- X3 passphrase vectors -------------------------------------------------------

def test_x3_no_text_is_a_code():
    # test_phone_passphrase.py pins the same: X3 never changes because of the code branch.
    for v in load_fixture(X3)["vectors"]:
        text = unhex(v["text_hex"])
        for form in (text, text.split(b"\0")[0], text.replace(b"\r", b""),
                     unhex(v["android_secret_hex"]), unhex(v["p4_stored_hex"])):
            assert not paircode.is_code(form), v["id"]


def test_x3_entry_policy():
    vectors = load_fixture(X3)["vectors"]
    assert len(vectors) == 32
    for v in vectors:
        text = unhex(v["text_hex"])
        forms: list[str | bytes] = [text]
        if v["valid_utf8"]:
            forms.append(text.decode("utf-8"))
        for entry in forms:
            result = paircode.pipe_secret(entry)
            if v["android_accepts"]:
                assert isinstance(result, PipeSecret), (v["id"], result)
                assert result.kind is SecretKind.PASSPHRASE and not result.is_code
                assert bytes(result.secret).hex() == v["android_secret_hex"], v["id"]
                assert v["parity"] == "same_secret" and v["p4_stored_hex"] == v["android_secret_hex"]
            else:
                assert isinstance(result, EntryRefused), v["id"]
                assert result.fault.value == v["android_reason"], (v["id"], result.fault)


def test_x3_psk():
    for v in load_fixture(X3)["vectors"]:
        if v["p4_result"] != "set":
            assert not v["psk_hex"], v["id"]
            continue
        # Also the P4-only cases (CR dropped by the serial reader, a stray 0xFF):
        # PBKDF2 runs over whatever bytes were stored.
        assert psk_hex(unhex(v["p4_stored_hex"])) == v["psk_hex"], v["id"]
        if v["android_accepts"]:
            result = paircode.pipe_secret(unhex(v["text_hex"]))
            assert isinstance(result, PipeSecret) and result.psk().hex() == v["psk_hex"], v["id"]


def test_valid_code_skips_class_rules():
    # firmware-009 §6.2 #7: a code needs no lowercase or punctuation.
    for entry in ("0123456789ABCDE4", "0123-4567-89AB-CDE4", "0123 4567 89ab cde4"):
        assert isinstance(paircode.passphrase_bytes(entry), EntryFault), entry
        result = paircode.pipe_secret(entry)
        assert isinstance(result, PipeSecret) and result.is_code, entry
        assert bytes(result.secret) == b"0123456789ABCDE4"
    # A spelling that would also pass the passphrase rule is still stored
    # canonical, as the P4's `phone secret` stores it (code check first).
    mixed = "0123-4567-89ab-CDE4"
    assert paircode.passphrase_bytes(mixed) == mixed.encode("ascii")
    result = paircode.pipe_secret(mixed)
    assert isinstance(result, PipeSecret) and bytes(result.secret) == b"0123456789ABCDE4"
    # A code with one wrong symbol falls through to the passphrase rule.
    refused = paircode.pipe_secret("0123456789ABCDE5")
    assert isinstance(refused, EntryRefused) and refused.fault is EntryFault.NO_LOWER


def test_entry_fault_order():
    assert paircode.passphrase_bytes(b"Aa1!aaaaaa\xff") is EntryFault.INVALID_UTF8
    assert paircode.passphrase_bytes("Aa1!aaa\ud800aaa") is EntryFault.INVALID_UTF8
    assert paircode.passphrase_bytes("Aa1!aaaa\naaa") is EntryFault.CONTROL
    assert paircode.passphrase_bytes("Aa1!aaaa\0aaa") is EntryFault.CONTROL
    assert paircode.passphrase_bytes("  Aa1!aaaa  ") is EntryFault.TOO_SHORT
    assert paircode.passphrase_bytes("") is EntryFault.TOO_SHORT
    assert paircode.passphrase_bytes("Aa1!" + "x" * 125) is EntryFault.TOO_LONG
    assert paircode.passphrase_bytes("aa1!aaaaaa") is EntryFault.NO_UPPER
    assert paircode.passphrase_bytes("AA1!AAAAAA") is EntryFault.NO_LOWER
    assert paircode.passphrase_bytes("Aa!!aaaaaa") is EntryFault.NO_DIGIT
    assert paircode.passphrase_bytes("Aa11aaaaaa") is EntryFault.NO_PUNCT
    assert paircode.passphrase_bytes(" \tAa1!aaaaaa\x0b ") == b"Aa1!aaaaaa"


# ---- PSK -------------------------------------------------------------------------------

def test_psk_label_and_rounds():
    golden = load_fixture(GOLDEN)
    assert golden["label_ascii"] == paircode.PIPE_LABEL and golden["pbkdf2_iterations"] == paircode.PBKDF2_ITERATIONS
    assert psk_hex(golden["passphrase_utf8"].encode("utf-8")) == golden["psk_hex"]
    # RFC 7914 §11 PBKDF2-HMAC-SHA256 vector (one block, c = 1).
    assert paircode.pbkdf2_sha256(b"passwd", b"salt", 1).hex() == \
        "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"


def test_psk_takes_bytes_only():
    try:
        paircode.psk("SyntheticPipe!123")  # type: ignore[arg-type]
    except TypeError:
        return
    raise AssertionError("psk() accepted a str")


def test_secret_repr_masks_and_wipe():
    result = paircode.pipe_secret("Zebra Quokka!123")
    assert isinstance(result, PipeSecret)
    assert "Zebra" not in repr(result) and "Quokka" not in repr(result) and "secret" not in repr(result)
    result.wipe()
    assert not any(result.secret) and len(result.secret) == len("Zebra Quokka!123")
    refused = paircode.pipe_secret("short")
    assert "short" not in repr(refused)
