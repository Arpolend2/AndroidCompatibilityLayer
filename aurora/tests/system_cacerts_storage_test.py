#!/usr/bin/env python3
import concurrent.futures
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def main():
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        sources = list((ROOT / "core" / "src").rglob("*.c"))
        sources += [p for p in (ROOT / "core" / "third_party" / "zlib").glob("*.c")
                    if not p.name.startswith("gz")]
        sources += [ROOT / "core" / "tests" / "mg_storage_tests.c",
                    ROOT / "core" / "tests" / "mg_gfx_standin.c",
                    ROOT / "aurora" / "tests" / "system_cacerts_storage_test.c"]
        flags = ["-std=gnu11", "-O0", "-ffunction-sections", "-fdata-sections", "-D_GNU_SOURCE",
                 "-I" + str(ROOT / "core" / "third_party" / "zlib")]

        def compile_one(item):
            index, source = item
            target = work / (str(index) + ".o")
            subprocess.run(["cc", *flags, "-c", str(source), "-o", str(target)], check=True)
            return str(target)

        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            objects = list(pool.map(compile_one, enumerate(sources)))
        binary = work / "storage-test"
        subprocess.run(["cc", "-Wl,--gc-sections", *objects, "-lm", "-lpthread", "-ldl",
                        "-o", str(binary)], check=True)
        scratch = work / "scratch"
        scratch.mkdir()
        subprocess.run([str(binary), str(scratch)], check=True)


if __name__ == "__main__":
    main()
