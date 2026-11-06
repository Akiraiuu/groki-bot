#!/usr/bin/env python3
"""Verify release contains the real MN5 model in the unchanged model partition."""
import hashlib
import json
from pathlib import Path
import sys

root = Path(sys.argv[1])
args = json.loads((root / 'flasher_args.json').read_text())
files = args['flash_files']
entry = next((v for k, v in files.items() if int(k, 0) == 0xDB0000), None)
assert entry, 'Model partition absent from flash manifest'
model = root / entry
data = model.read_bytes()
assert 2_000_000 < len(data) <= 0x240000, 'Missing/oversized real model'
assert b'mn5q8_cn\0' in data[:4096], 'Expected Chinese MN5 model header'
assert len((root / 'stackchan_idf.bin').read_bytes()) <= 0x400000, 'App exceeds OTA slot'
report = {'model': entry, 'offset': '0xdb0000', 'partition_size': 0x240000,
          'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
          'hardware_recognition_verified': False}
(root / 'wake-model-validation.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report))
