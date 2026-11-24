"""Exercise the MediaCodec overlay's error path on a JVM with host doubles."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--output", type=Path, required=True, help="isolated directory for generated files")
args = parser.parse_args()
out = args.output.resolve()
if out.exists() and any(out.iterdir()):
    raise SystemExit("output directory must be empty")
for name in ("stubs", "framework", "mocks"):
    (out / name).mkdir(parents=True, exist_ok=True)

jdk = os.environ.get("MEGLER_JDK")
suffix = ".exe" if os.name == "nt" else ""
javac = str(Path(jdk) / "bin" / ("javac" + suffix)) if jdk else shutil.which("javac")
java = str(Path(jdk) / "bin" / ("java" + suffix)) if jdk else shutil.which("java")
if not javac or not java:
    raise SystemExit("JDK not found; set MEGLER_JDK or add it to PATH")

def run(*command):
    subprocess.run([str(x) for x in command], cwd=root, check=True)

run(sys.executable, root / "tools/framework-sources.py", root / "core/framework",
    root / "core/framework-overlay", out)
run(javac, "-nowarn", "-source", "8", "-target", "8", "-encoding", "UTF-8",
    "-d", out / "stubs", "@" + str(out / "stubs.arg"))
run(javac, "-nowarn", "-source", "8", "-target", "8", "-encoding", "UTF-8",
    "-cp", out / "stubs", "-d", out / "framework", "@" + str(out / "sources.arg"))
tests = sorted((root / "core/tests/media-codec-errors").rglob("*.java"))
run(javac, "-encoding", "UTF-8", "-cp", out / "framework", "-d", out / "mocks",
    root / "core/tests/app/media/src/megler/test/media/Main.java", *tests)
run(java, "-cp", str(out / "mocks") + os.pathsep + str(out / "framework"), "TestCodec")
run(java, "-cp", str(out / "mocks") + os.pathsep + str(out / "framework"), "megler.test.media.VideoFramesTest")
