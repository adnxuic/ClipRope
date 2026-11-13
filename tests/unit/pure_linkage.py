"""Inspect the generated Visual Studio projects and the imports of their actual test images."""
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET


def projects(project, visited):
    if project in visited:
        return
    visited.add(project)
    for reference in ET.parse(project).findall(".//{*}ItemGroup/{*}ProjectReference[@Include]"):
        projects((project.parent / reference.attrib["Include"]).resolve(), visited)


def main(tree, linker, images):
    allowed = {"ZERO_CHECK", "cliprope_build_session", "cliprope_geometry", "cliprope_application",
               "cliprope_domain", "cliprope_localization"}
    native = {b"user32.dll", b"ole32.dll", b"combase.dll", b"d2d1.dll", b"dwrite.dll", b"sqlite3.dll",
              b"windowscodecs.dll", b"shell32.dll"}
    for image in images:
        graph = set()
        projects(tree / (image.stem + ".vcxproj"), graph)
        targets = {p.stem for p in graph}
        expected = "cliprope_application" if image.stem == "cliprope_invocation_unit" else "cliprope_geometry"
        extra = targets - allowed - {image.stem}
        if extra or expected not in targets:
            raise ValueError(f"Unexpected {image.stem} target graph: {sorted(targets)}")
        imports = subprocess.check_output([linker, "/dump", "/dependents", image])
        names = set(re.findall(rb"(?im)^\s*([\w.-]+\.dll)\s*$", imports.lower()))
        if not names or names & native:
            raise ValueError(f"Unexpected {image.stem} DLL imports: {sorted(names)}")
        print(f"{image.stem} projects={sorted(targets)} DLLs={sorted(n.decode() for n in names)}")


if __name__ == "__main__":
    main(Path(sys.argv[1]), sys.argv[2], [Path(arg) for arg in sys.argv[3:]])
