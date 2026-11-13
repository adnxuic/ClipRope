"""Check the dependencies of the actual history-only client."""
from pathlib import Path
import sys
import subprocess

from pure_linkage import projects


def main(tree, linker, executable):
    graph = set()
    projects(tree / (executable.stem + ".vcxproj"), graph)
    allowed = {"cliprope_history_link", "cliprope_history", "cliprope_storage", "cliprope_database",
               "cliprope_domain", "cliprope_search_text", "cliprope_file_codec", "cliprope_sqlite",
               "cliprope_build_session", "ZERO_CHECK"}
    names = {project.stem for project in graph}
    if "cliprope_history" not in names or names - allowed:
        raise ValueError(f"Unexpected history project references: {sorted(names)}")
    imports = subprocess.check_output([linker, "/dump", "/dependents", executable]).lower()
    forbidden = (b"windowscodecs.dll", b"dwrite.dll", b"d2d1.dll")
    if any(name in imports for name in forbidden):
        raise ValueError("History-only executable imports graphics or preview codecs")
    print("History-only closure:", ", ".join(sorted(names)))


if __name__ == "__main__":
    main(Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3]))
