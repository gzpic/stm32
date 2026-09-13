"""Generate the standalone FreeRTOS Keil project without modifying the bare-metal project."""
from pathlib import Path
import os
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
source = root / "stm32/i2c_slave.uvprojx"
destination = root / "stm32/freertos_i2c.uvprojx"
tree = ET.parse(source)


def relative(path):
    return os.path.relpath(path, destination.parent).replace("/", "\\")


def project_path(text):
    return (source.parent / text.replace("\\", "/")).resolve()


for element in tree.iter("FilePath"):
    path = project_path(element.text)
    if path == (root / "stm32/main.c").resolve():
        element.text = relative(root / "stm32/freertos_main.c")
    elif path == (root / "vendor/hal_example/User/stm32f4xx_it.c").resolve():
        element.text = relative(root / "stm32/freertos_stm32f4xx_it.c")

for element in tree.iter("IncludePath"):
    if element.text:
        element.text += ";" + ";".join([
            relative(root / "freertos_kernel/include"),
            relative(root / "freertos_kernel/portable/GCC/ARM_CM4F"),
        ])

for tag, value in [("TargetName", "Jetson_FreeRTOS_I2C"),
                   ("OutputName", "freertos_i2c"),
                   ("OutputDirectory", ".\\OutputFreeRTOS\\"),
                   ("ListingPath", ".\\OutputFreeRTOS\\")]:
    for element in tree.iter(tag):
        element.text = value

groups = tree.find(".//Groups")
group = ET.SubElement(groups, "Group")
ET.SubElement(group, "GroupName").text = "FreeRTOS kernel"
files = ET.SubElement(group, "Files")
for path in [
    root / "freertos_kernel/list.c",
    root / "freertos_kernel/queue.c",
    root / "freertos_kernel/tasks.c",
    root / "freertos_kernel/portable/GCC/ARM_CM4F/port.c",
    root / "freertos_kernel/portable/MemMang/heap_4.c",
    root / "stm32/freertos_hooks.c",
]:
    file = ET.SubElement(files, "File")
    ET.SubElement(file, "FileName").text = path.name
    ET.SubElement(file, "FileType").text = "1"
    ET.SubElement(file, "FilePath").text = relative(path)

tree.write(destination, encoding="UTF-8", xml_declaration=True)
for element in tree.iter("FilePath"):
    path = destination.parent / element.text.replace("\\", "/")
    if not path.is_file():
        raise SystemExit(f"Missing project source: {path}")
print(f"Generated and checked: {destination}")
