"""Bounded synthetic bytes for the restored public APIs; no retail inputs."""
import hashlib
from pathlib import Path
import stat
import subprocess
import sys
import uuid
import zipfile

exe, destination = sys.argv[1:]
root = Path(destination)/str(uuid.uuid4())
root.mkdir(parents=True)
original=b"original synthetic image"*8
payload=b"shared archive payload\0"*37
(root/'original.bin').write_bytes(original)
(root/'payload.bin').write_bytes(payload)
manifest=f'''format_version = 1
id = "shared"
version = "1"
name = "Synthetic shared package"
[[target]]
game_id = "fixture"
rom_sha256 = "{hashlib.sha256(original).hexdigest()}"
[[feature]]
id = "main"
name = "Fixture"
default_enabled = true
'''
for name, method in [('stored',zipfile.ZIP_STORED),('deflated',zipfile.ZIP_DEFLATED),('corrupt',zipfile.ZIP_STORED)]:
    with zipfile.ZipFile(root/(name+'.rmod'),'w',compression=method) as archive:
        archive.writestr('manifest.toml',manifest)
        archive.writestr('assets/雪.txt',payload)
for name, path, attributes in [('symlink','assets/link',((stat.S_IFLNK|0o777)<<16)),
    ('reparse','assets/link',0x400),('traversal','../escape.txt',0)]:
    with zipfile.ZipFile(root/(name+'.rmod'),'w') as archive:
        archive.writestr('manifest.toml',manifest)
        entry=zipfile.ZipInfo(path);entry.create_system=3;entry.external_attr=attributes
        archive.writestr(entry,b'outside')
data=bytearray((root/'corrupt.rmod').read_bytes());data[data.index(payload)]^=1
(root/'corrupt.rmod').write_bytes(data)
subprocess.run([exe,str(root)],check=True)
assert (root/'original.bin').read_bytes()==original
assert not (root/'escape.txt').exists()
print('Synthetic original input unchanged; path/content identity and native memory-write contracts exercised')
