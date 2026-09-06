"""Bundle headers and project-referenced sources from the original local example.

Run only when refreshing vendor dependencies. Bytes and copyright notices are
preserved; firmware binaries and debugger/user settings are never imported.
"""
from pathlib import Path
import shutil
import sys
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
base = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else (
    root.parent / "2，标准例程-HAL库版本/实验1 跑马灯实验")
project = base / "Projects/MDK-ARM/atk_f407.uvprojx"
destination = root / "vendor/hal_example"
files = set(base.rglob("*.h"))
files.add(project)
for element in ET.parse(project).iter("FilePath"):
    files.add((project.parent / element.text.replace("\\", "/")).resolve())
for path in sorted(files):
    target = destination / path.relative_to(base)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(path, target)
print(f"Imported {len(files)} original files into {destination}")
