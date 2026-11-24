#!/usr/bin/env python3
import concurrent.futures
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def run(*args, **kwargs):
    return subprocess.run(args, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True, **kwargs).stdout.strip()


def cert(directory, name, subject):
    key = directory / (name + ".key")
    out = directory / (name + ".pem")
    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
        "-subj", subject, "-keyout", str(key), "-out", str(out))
    return out.read_bytes()


def fingerprint(pem):
    result = subprocess.run(["openssl", "x509", "-outform", "DER"], input=pem,
                            check=True, stdout=subprocess.PIPE)
    return hashlib.sha256(result.stdout).hexdigest()


def subject_hash(pem):
    result = subprocess.run(["openssl", "x509", "-noout", "-subject_hash_old"], input=pem,
                            check=True, stdout=subprocess.PIPE)
    return result.stdout.decode().strip()


def main():
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        binary = work / "test"
        run("cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            str(ROOT / "aurora" / "system_cacerts.c"),
            str(ROOT / "aurora" / "tests" / "system_cacerts_test.c"),
            "-lcrypto", "-o", str(binary))
        builtins = work / "android" / "etc" / "security" / "cacerts"
        builtins.mkdir(parents=True)
        data = work / "data"
        data.mkdir()
        a = cert(work, "a", "/CN=Collision")
        b = cert(work, "b", "/CN=Collision")
        c = cert(work, "c", "/CN=System only")
        assert subject_hash(a) == subject_hash(b)
        (builtins / (subject_hash(a) + ".0")).write_bytes(a)
        (builtins / (subject_hash(b) + ".1")).write_bytes(b)
        bundle = work / "bundle.pem"
        deny = work / "blacklist.pem"
        policy = work / "policy.txt"
        policy.write_text("[p11-kit-object-v1]\nnss-server-distrust-after: \"%00\"\n")
        bundle.write_bytes(a + b + c + c)
        deny.write_bytes(b)

        def prepare(bundle_path=bundle):
            return run(str(binary), str(work / "android"), str(data), str(bundle_path), str(deny), str(policy))

        store = Path(prepare())
        files = sorted(store.iterdir())
        assert len(files) == 2
        assert {fingerprint(f.read_bytes()) for f in files} == {fingerprint(a), fingerprint(c)}
        assert (store / (subject_hash(a) + ".0")).is_file()
        assert (store / (subject_hash(c) + ".0")).is_file()
        assert prepare() == str(store)

        deny.write_bytes(b"")
        updated = Path(prepare())
        assert updated != store and len(list(updated.iterdir())) == 3
        assert (updated / (subject_hash(a) + ".1")).is_file()
        assert len(list(store.iterdir())) == 2
        fallback = Path(prepare(work / "missing.pem"))
        assert {fingerprint(f.read_bytes()) for f in fallback.iterdir()} == {fingerprint(a), fingerprint(b)}
        deny.write_bytes(b)
        filtered_fallback = Path(prepare(work / "missing.pem"))
        assert {fingerprint(f.read_bytes()) for f in filtered_fallback.iterdir()} == {fingerprint(a)}
        deny.write_bytes(b"")

        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(lambda _: prepare(), range(16)))
        assert set(results) == {str(updated)}
        assert not list((data / "system-cacerts").glob(".tmp-*"))

        policy.write_bytes(b'[p11-kit-object-v1]\nnss-server-distrust-after: "241130235959Z"\n' +
                           b + b'\n[p11-kit-object-v1]\nnss-server-distrust-after: "250415235959Z"\n' + c)
        restricted = Path(prepare())
        assert {fingerprint(f.read_bytes()) for f in restricted.iterdir()} == {fingerprint(a), fingerprint(b)}
        deny.write_bytes(b)
        restricted_blacklist = Path(prepare())
        assert {fingerprint(f.read_bytes()) for f in restricted_blacklist.iterdir()} == {fingerprint(a)}
        deny.write_bytes(b"")
        policy.write_text("[p11-kit-object-v1]\nnss-server-distrust-after: \"%00\"\n")

        if shutil.which("trust"):
            result = run(str(binary), str(work / "android"), str(data), str(bundle), "-", "-")
            assert len(list(Path(result).iterdir())) == 3

        bundle.write_bytes(a + b"-----BEGIN CERTIFICATE-----\nbroken\n")
        failure = subprocess.run([str(binary), str(work / "android"), str(data),
                                  str(bundle), str(deny), str(policy)], capture_output=True, text=True)
        assert failure.returncode != 0 and "incomplete certificate" in failure.stderr
        assert not list((data / "system-cacerts").glob(".tmp-*"))
        bundle.write_bytes(a + b + c)
        deny.write_text("invalid blacklist")
        failure = subprocess.run([str(binary), str(work / "android"), str(data),
                                  str(bundle), str(deny), str(policy)], capture_output=True, text=True)
        assert failure.returncode != 0 and "no certificates" in failure.stderr
        deny.write_bytes(b"")
        next(updated.iterdir()).write_text("tampered")
        failure = subprocess.run([str(binary), str(work / "android"), str(data),
                                  str(bundle), str(deny), str(policy)], capture_output=True, text=True)
        assert failure.returncode != 0 and "does not match" in failure.stderr
        print("system cacerts tests passed")


if __name__ == "__main__":
    main()
