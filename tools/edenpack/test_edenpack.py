import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

MODULE = Path(__file__).with_name("edenpack.py")
SPEC = importlib.util.spec_from_file_location("edenpack", MODULE)
edenpack = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(edenpack)


class EdenPackTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.game = self.root / "Game"
        (self.game / "ExeFS").mkdir(parents=True)
        (self.game / "RomFS" / "Data" / "Nested").mkdir(parents=True)
        (self.game / "ExeFS" / "main").write_bytes(b"executable\0payload")
        (self.game / "ExeFS" / "empty").write_bytes(b"")
        (self.game / "RomFS" / "Data" / "Nested" / "asset.bin").write_bytes(bytes(range(256)) * 100)
        self.archive = self.root / "game.edenpack"

    def tearDown(self):
        self.tmp.cleanup()

    def test_round_trip_and_source_verification(self):
        result = edenpack.pack(self.game, self.archive)
        self.assertEqual(result["files"], 3)
        checked = edenpack.verify(self.archive, self.game)
        self.assertTrue(checked["valid"])
        self.assertEqual(checked["verified_bytes"], sum(p.stat().st_size for p in self.game.rglob("*") if p.is_file()))

    def test_indexed_ranges_are_aligned(self):
        edenpack.pack(self.game, self.archive)
        index, _ = edenpack.read_index(self.archive)
        for entry in index["entries"]:
            if entry["type"] == "file":
                self.assertEqual(entry["offset"] % edenpack.ALIGNMENT, 0)

    def test_corrupt_payload_is_detected(self):
        edenpack.pack(self.game, self.archive)
        index, _ = edenpack.read_index(self.archive)
        first = next(e for e in index["entries"] if e["type"] == "file" and e["size"])
        with self.archive.open("r+b") as stream:
            stream.seek(first["offset"])
            byte = stream.read(1)
            stream.seek(first["offset"])
            stream.write(bytes([byte[0] ^ 0xFF]))
        with self.assertRaises(edenpack.EdenPackError):
            edenpack.verify(self.archive)

    def test_rejects_missing_required_directory(self):
        (self.game / "RomFS").rename(self.game / "RomFS-away")
        with self.assertRaises(edenpack.EdenPackError):
            edenpack.pack(self.game, self.archive)

    def test_rejects_path_traversal_in_index(self):
        edenpack.pack(self.game, self.archive)
        index, _ = edenpack.read_index(self.archive)
        index["entries"][0]["path"] = "../escape"
        payload = json.dumps(index, separators=(",", ":"), sort_keys=True).encode()
        with self.archive.open("r+b") as stream:
            stream.seek(0, 2)
            index_offset = stream.tell()
            stream.write(payload)
            stream.seek(0)
            stream.write(edenpack.HEADER.pack(edenpack.MAGIC, edenpack.VERSION, 0, index_offset, len(payload), len(index["entries"])))
        with self.assertRaises(edenpack.EdenPackError):
            edenpack.read_index(self.archive)


if __name__ == "__main__":
    unittest.main()
