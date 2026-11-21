import os
import re
import signal
import subprocess
import sys
import tempfile
import time

SERVICE = "ru.androidcompatibilitylayer.appmanager"


def bus_call(member, argument):
    return subprocess.check_output(
        ["dbus-send", "--session", "--print-reply", "--dest=org.freedesktop.DBus",
         "/org/freedesktop/DBus", "org.freedesktop.DBus." + member, "string:" + argument],
        universal_newlines=True)


def has_owner(name):
    reply = bus_call("NameHasOwner", name)
    if "boolean true" in reply:
        return True
    if "boolean false" in reply:
        return False
    raise RuntimeError("Unexpected NameHasOwner reply: " + reply)


def owner_pid():
    reply = bus_call("GetConnectionUnixProcessID", SERVICE)
    match = re.search(r"\buint32\s+(\d+)", reply)
    if not match:
        raise RuntimeError("Unexpected process ID reply: " + reply)
    return int(match.group(1))


def client(binary, *arguments):
    result = subprocess.check_output([binary] + list(arguments), universal_newlines=True)
    if arguments:
        return None
    match = re.fullmatch(r"PASS sender=(:\S+) instance=(\{\S+\})\n", result)
    if not match:
        raise RuntimeError("Unexpected client output: " + result)
    return match.groups()


def inside(mode, service, client_binary):
    assert not has_owner(SERVICE), "probe unexpectedly running"
    process = None
    activated_pid = None
    try:
        if mode == "running":
            process = subprocess.Popen([service])
            for _ in range(100):
                if has_owner(SERVICE):
                    break
                if process.poll() is not None:
                    raise RuntimeError("Probe exited before registering")
                time.sleep(0.05)
            assert has_owner(SERVICE), "probe did not register"
            assert owner_pid() == process.pid, "service is not the spawned process"
        first = client(client_binary)
        if mode == "activation":
            activated_pid = owner_pid()
            assert os.path.realpath("/proc/{}/exe".format(activated_pid)) == os.path.realpath(service)
        second = client(client_binary)
        client(client_binary, "--invalid")
        assert first[0] != second[0], "separate client processes have the same sender"
        assert first[1] == second[1], "replies came from different instances"
        print("PASS {}: separate processes, two actual senders and rejected spoof".format(mode))
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
        if activated_pid is not None:
            try:
                if os.path.realpath("/proc/{}/exe".format(activated_pid)) == os.path.realpath(service):
                    os.kill(activated_pid, signal.SIGTERM)
            except ProcessLookupError:
                pass


def main():
    if len(sys.argv) == 5 and sys.argv[1] == "--inside":
        inside(sys.argv[2], sys.argv[3], sys.argv[4])
        return
    if len(sys.argv) != 3:
        raise SystemExit("usage: test.py SERVICE CLIENT")
    service, client_binary = map(os.path.realpath, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="acl-microg-dbus-") as directory:
        service_dir = os.path.join(directory, "dbus-1", "services")
        os.makedirs(service_dir)
        with open(os.path.join(service_dir, SERVICE + ".service"), "w") as file:
            file.write("[D-BUS Service]\nName={}\nExec={}\n".format(SERVICE, service))
        env = os.environ.copy()
        env["XDG_DATA_HOME"] = directory
        env["ACL_MICROG_ISOLATED_TEST"] = "1"
        for mode in ("running", "activation"):
            subprocess.check_call(
                ["dbus-run-session", "--", sys.executable, __file__, "--inside", mode,
                 service, client_binary], env=env)


if __name__ == "__main__":
    main()
