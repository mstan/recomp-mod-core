"""Synthetic package bytes only; no retail images or extracted game data."""
import hashlib
import pathlib
import subprocess
import sys
import uuid
import zipfile

exe, destination = sys.argv[1:]
root = pathlib.Path(destination) / str(uuid.uuid4())
root.mkdir(parents=True, exist_ok=True)
image = bytes(range(256)) * 4096 + bytes(range(57))
(root / "original.bin").write_bytes(image)
(root / "wrong.bin").write_bytes(image + b"wrong")
digest = hashlib.sha256(image).hexdigest()


def manifest(package, feature="main", plugin="", requires="", extra=""):
    text = f'''format_version = 1
id = "{package}"
version = "1"
name = "Synthetic package"
[[target]]
game_id = "fixture"
rom_sha256 = "{digest}"
[[feature]]
id = "{feature}"
name = "Feature"
default_enabled = false
'''
    if requires:
        text += f'requires = "{requires}"\n'
    if plugin:
        text += f'[[plugin]]\nfeature = "{feature}"\nid = "{plugin}"\n'
    return text + extra


def package(name, text, method=zipfile.ZIP_DEFLATED, contents=None):
    with zipfile.ZipFile(root / f"{name}.rmod", "w", compression=method) as archive:
        archive.writestr("manifest.toml", text)
        archive.writestr("assets/note.txt", b"portable content")
        for path, content in (contents or {}).items():
            archive.writestr(path, content)


package("base", manifest("base", plugin="base.plugin", extra='''[[option]]
feature = "main"
id = "level"
label = "Level"
type = "integer"
min = 1
max = 9
step = 2
default = 3
'''))
package("dependent", manifest("dependent", requires="base/main"), zipfile.ZIP_STORED)
package("conflict", manifest("conflict", plugin="conflict.plugin"))
package("resource", manifest("resource", extra=f'''[[resource]]
feature = "main"
id = "donor"
label = "Original image"
format = "file"
sha256 = "{digest}"
size = {len(image)}
required = true
'''))
package("traversal", manifest("traversal"), contents={"../escape.txt": b"bad"})
package("invalid", 'format_version = 9\nid = "invalid"\n')
package("corrupt", manifest("corrupt"), zipfile.ZIP_STORED)
data = bytearray((root / "corrupt.rmod").read_bytes())
offset = data.index(b"portable content")
data[offset] ^= 1
(root / "corrupt.rmod").write_bytes(data)
subprocess.run([exe, str(root), digest], check=True)
assert (root / "original.bin").read_bytes() == image
assert not (root / "escape.txt").exists()
print("original image unchanged; archive containment preserved")
