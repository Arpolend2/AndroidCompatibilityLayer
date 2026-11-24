"""Build only the synthetic camera test APK; never rebuild the shared framework."""
import base64
import importlib.util
from pathlib import Path
import subprocess
import sys
import zipfile

repo = Path(__file__).resolve().parents[2]
apk = Path(sys.argv[1]).resolve()
apk.parent.mkdir(parents=True, exist_ok=True)
dex = apk.with_suffix(".dex")
subprocess.run([
    sys.executable, str(repo / "tools" / "dexasm.py"), "asm", "-o", str(dex),
    str(Path(__file__).with_name("camera_storage_media.smali")),
], check=True)
spec = importlib.util.spec_from_file_location("manifest", repo / "tools" / "make-test-apk.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)
builder.ATTR_IDS["screenOrientation"] = 0x0101001E
m = builder.Manifest()
m.start("manifest", [("package", "s", "org.example.camera")])
m.start("uses-sdk", [("minSdkVersion", "i", 21), ("targetSdkVersion", "i", 22)])
m.end("uses-sdk")
for permission in ["android.permission.CAMERA", "android.permission.WRITE_EXTERNAL_STORAGE"]:
    m.start("uses-permission", [("name", "s", permission)])
    m.end("uses-permission")
m.start("application", [("hasCode", "b", True)])
m.start("activity", [("name", "s", "megler.CameraStorageFixture"), ("screenOrientation", "i", 0)])
m.start("intent-filter", [])
m.start("action", [("name", "s", "android.intent.action.MAIN")])
m.end("action")
m.start("category", [("name", "s", "android.intent.category.LAUNCHER")])
m.end("category")
m.end("intent-filter")
m.end("activity")
m.end("application")
m.end("manifest")
png = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aKQAAAABJRU5ErkJggg==")
with zipfile.ZipFile(apk, "w") as z:
    z.writestr("AndroidManifest.xml", m.build())
    z.write(dex, "classes.dex")
    z.writestr("assets/fixture.png", png)
