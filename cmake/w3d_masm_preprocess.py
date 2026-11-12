"""Minimal C-preprocessor for the Vulkan-Loader MARMASM assembly.

The assembler CMake picks for Windows ARM64 (armasm64) has no C
preprocessor, but the loader's trampoline .asm branches on
`#if defined(_ARM64EC_)` (plus #define lines). This evaluates those
branches for the requested -D definitions (native arm64 builds define
none), drops the preprocessor lines, and emits plain MASM source.
Only the constructs the loader's asm actually uses are supported.
"""
import re
import sys


def preprocess(src_text, defines):
    out = []
    stack = []  # entries: (this_branch_taken, parent_taken)
    for line in src_text.splitlines(keepends=True):
        s = line.strip()
        if s.startswith("#if "):
            m = re.match(r"defined\(\s*([A-Za-z_]\w*)\s*\)", s[4:].strip())
            defined = bool(m) and m.group(1) in defines
            parent = stack[-1][1] if stack else True
            stack.append((parent and defined, parent))
        elif s.startswith("#else"):
            if stack:
                taken, parent = stack.pop()
                stack.append((parent and not taken, parent))
        elif s.startswith("#endif"):
            if stack:
                stack.pop()
        elif s.startswith("#"):
            continue  # #define, #include, others: not needed in this pass
        else:
            if not (stack and not stack[-1][0]):
                out.append(line)
    return "".join(out)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    defines = set()
    for a in sys.argv[3:]:
        if a.startswith("-D"):
            defines.add(a[2:])
    with open(src, "r", encoding="utf-8") as f:
        text = f.read()
    with open(dst, "w", encoding="utf-8", newline="\r\n") as f:
        f.write(preprocess(text, defines))


if __name__ == "__main__":
    main()
