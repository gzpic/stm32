"""Mechanically adapt the existing Keil project, preserving its vendor settings."""
from pathlib import Path
import os
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
base = root / "vendor/hal_example"
source = base / "Projects/MDK-ARM/atk_f407.uvprojx"
destination = root / "stm32/spi_slave.uvprojx"
tree = ET.parse(source)

def relative(path):
    return os.path.relpath(path, destination.parent).replace("/", "\\")

def old_path(text):
    return (source.parent / text.replace("\\", "/")).resolve()

for element in tree.iter("FilePath"):
    path = old_path(element.text)
    element.text = relative(root / "stm32/main.c" if path == base / "User/main.c" else path)
for element in tree.iter("IncludePath"):
    if element.text:
        element.text = ";".join(relative(old_path(p)) for p in element.text.split(";")
                                if old_path(p).is_dir())
        element.text += ";.;" + relative(root / "common")
for tag, text in [("TargetName", "Jetson_SPI_Slave"), ("OutputName", "spi_slave"),
                  ("OutputDirectory", ".\\Output\\"), ("ListingPath", ".\\Output\\")]:
    for element in tree.iter(tag):
        element.text = text
groups = tree.find(".//Groups")
group = ET.SubElement(groups, "Group")
ET.SubElement(group, "GroupName").text = "SPI protocol and slave"
files = ET.SubElement(group, "Files")
for path in [root / "stm32/spi_slave.c", root / "common/protocol.c", root / "common/service.c", root / "common/commands.c"]:
    f = ET.SubElement(files, "File")
    ET.SubElement(f, "FileName").text = path.name
    ET.SubElement(f, "FileType").text = "1"
    ET.SubElement(f, "FilePath").text = relative(path)
tree.write(destination, encoding="UTF-8", xml_declaration=True)
for element in tree.iter("FilePath"):
    path = destination.parent / element.text.replace("\\", "/")
    if not path.is_file():
        raise SystemExit(f"Missing project source: {path}")
print(f"Generated and checked: {destination}")
