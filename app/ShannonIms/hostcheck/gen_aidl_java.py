#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Host stand-ins for the AIDL Java backend of our own interfaces.

    gen_aidl_java.py AIDL_DIR OUT_DIR

Parcelables become classes with public fields, @Backing(int) enums classes of
int constants, interfaces Java interfaces with DESCRIPTOR, VERSION, HASH and a
Stub (asInterface, asBinder) as the AIDL compiler generates them. Only for
the ShannonIms host check: nothing here runs.
"""
import re
import sys
from pathlib import Path

PRIM = {"int": "int", "long": "long", "boolean": "boolean", "byte": "byte", "char": "char",
        "float": "float", "double": "double", "void": "void", "String": "String"}


def jtype(t):
    t = re.sub(r"@\w+(\([^)]*\))?\s*", "", t).strip()
    arr = t.endswith("[]")
    base = t[:-2] if arr else t
    base = PRIM.get(base, base)
    return base + ("[]" if arr else "")


def strip(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def main(src, out):
    enums = {re.search(r"\benum\s+(\w+)", strip(f.read_text())).group(1)
             for f in Path(src).rglob("*.aidl") if re.search(r"\benum\s+\w+", strip(f.read_text()))}
    for f in Path(src).rglob("*.aidl"):
        text = strip(f.read_text())
        pkg = re.search(r"package\s+([\w.]+);", text).group(1)
        m = re.search(r"\b(enum|parcelable|interface)\s+(\w+)", text)
        kind, name = m.group(1), m.group(2)
        body = text[text.index("{", m.end()) + 1:text.rindex("}")]
        lines = [f"package {pkg};"]
        if kind == "enum":
            lines.append(f"public @interface {name} {{")
            for i, item in enumerate(x.strip() for x in body.split(",") if x.strip()):
                lines.append(f"    public static final int {item} = {i};")
            lines.append("}")
        elif kind == "parcelable":
            lines.append(f"public class {name} {{")
            for stmt in (s.strip() for s in body.split(";") if s.strip()):
                fm = re.match(r"(.+?)\s+(\w+)\s*(?:=\s*(.+))?$", stmt, re.S)
                t, n, d = jtype(fm.group(1)), fm.group(2), fm.group(3)
                if t not in PRIM.values() and not t.endswith("[]") and t.split(".")[-1] in enums:
                    t = "int"
                if d:
                    d = re.sub(r"^[\w.]+\.(\w+)\.(\w+)$", r"\1.\2", d.strip())
                    lines.append(f"    public {t} {n} = {d};")
                else:
                    lines.append(f"    public {t} {n};")
            lines.append("}")
        else:
            lines.append(f"public interface {name} extends android.os.IInterface {{")
            lines.append(f'    public static final String DESCRIPTOR = "{pkg}.{name}";')
            lines.append("    public static final int VERSION = 1;")
            lines.append('    public static final String HASH = "notfrozen";')
            for stmt in (s.strip() for s in body.split(";") if s.strip()):
                if stmt.startswith("const "):
                    cm = re.match(r"const\s+(\S+)\s+(\w+)\s*=\s*(.+)$", stmt)
                    lines.append(f"    public static final {jtype(cm.group(1))} {cm.group(2)} = {cm.group(3)};")
                    continue
                mm = re.match(r"(?:oneway\s+)?(\S+)\s+(\w+)\s*\((.*)\)$", stmt, re.S)
                params = []
                for p in filter(None, (x.strip() for x in mm.group(3).split(","))):
                    p = re.sub(r"^(in|out|inout)\s+", "", p)
                    pt, pn = p.rsplit(None, 1)
                    params.append(f"{jtype(pt)} {pn}")
                lines.append(f"    public {jtype(mm.group(1))} {mm.group(2)}({', '.join(params)}) "
                             "throws android.os.RemoteException;")
            lines += [
                "    public int getInterfaceVersion() throws android.os.RemoteException;",
                "    public String getInterfaceHash() throws android.os.RemoteException;",
                f"    public static abstract class Stub implements {name} {{",
                f"        public static {name} asInterface(android.os.IBinder b) {{ return null; }}",
                "        public android.os.IBinder asBinder() { return null; }",
                "        public int getInterfaceVersion() { return VERSION; }",
                "        public String getInterfaceHash() { return HASH; }",
                "    }",
                "}",
            ]
        text_out = "\n".join(lines) + "\n"
        target = Path(out) / pkg.replace(".", "/") / f"{name}.java"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text_out)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
