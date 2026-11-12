"""Write a .def file from a DLL's export table (for `lib /def:` import libs).

Parses the "Ordinal Hint RVA Name" rows from `dumpbin /exports <dll>` and
emits a LIBRARY/EXPORTS definition file. Used for SDL3, whose Windows release
ships the DLL without an import library.
"""
import re
import subprocess
import sys


def main():
    dll, def_out = sys.argv[1], sys.argv[2]
    out = subprocess.run(
        ["dumpbin", "/nologo", "/exports", dll],
        capture_output=True, text=True, check=True).stdout
    names = []
    for line in out.splitlines():
        # Export table rows look like: "  636 27B 00032700 SDL_Init"
        # (ordinal decimal, hint HEX, RVA, name)
        m = re.match(
            r"^\s*(\d+)\s+([0-9A-Fa-f]+)\s+([0-9A-Fa-f]+)\s+(\S+)$", line)
        if m:
            names.append(m.group(4))
    if not names:
        sys.exit("make_import_def: no exports found in " + dll)
    with open(def_out, "w", encoding="ascii", newline="\r\n") as f:
        f.write("LIBRARY {}\nEXPORTS\n".format(dll.split("\\")[-1].replace(".dll", "")))
        for n in names:
            f.write("    {}\n".format(n))
    print("make_import_def: {} exports -> {}".format(len(names), def_out))


if __name__ == "__main__":
    main()
