import array
import os
import select
import socket
import struct
import subprocess
import sys
import time

HEADER = struct.Struct("!4sIQIII")


def packet(kind, channel=0, uid=0, pid=0, package=b""):
    return HEADER.pack(b"MGR1", kind, channel, uid, pid, len(package)) + package


def receive(sock, expected, channel, timeout=90):
    sock.settimeout(timeout)
    data = sock.recv(4096)
    if data != packet(expected, channel):
        raise AssertionError("unexpected control packet: " + repr(data))


def wait_line(process, expected=b"BOUND\n"):
    ready, _, _ = select.select([process.stdout], [], [], 90)
    if not ready:
        raise AssertionError("client timed out")
    line = process.stdout.readline()
    if line != expected:
        raise AssertionError("client did not bind: " + repr(line))


def main():
    worker, apk, data, root, apps, java, classpath = sys.argv[1:]
    with open(os.path.join(apps, "apps.txt"), "x", encoding="utf-8") as app_list:
        for channel in (1, 2):
            app_list.write("example.client%d\t\tClient\t1.0\t1\n" % channel)
    control, peer = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    child = os.fork()
    if child == 0:
        control.close()
        os.dup2(peer.fileno(), 3)
        os.set_inheritable(3, True)
        os.execv(worker, [worker, "--headless", "--control-fd=3", "--protocol-version=1",
                          "--apk=" + apk, "--data-dir=" + data,
                          "--android-root=" + root, "--apps-dir=" + apps])
    peer.close()
    clients = []
    try:
        receive(control, 1, 0, 120)
        if os.environ.get("WORKER_TEST_LIMIT") == "1":
            channels = []
            try:
                for channel in range(1, 130):
                    server, client = socket.socketpair()
                    control.sendmsg([packet(2, channel, 11000 + channel, 50000 + channel,
                                            ("example.limit%d" % channel).encode())],
                                    [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [server.fileno()]))])
                    server.close()
                    receive(control, 5 if channel <= 128 else 6, channel)
                    channels.append(client)
                control.send(packet(4))
                receive(control, 5, 0, 60)
                for client in channels:
                    client.settimeout(10)
                    if client.recv(1) != b"":
                        raise AssertionError("STOP left a channel open")
            finally:
                for client in channels:
                    client.close()
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                pid, status = os.waitpid(child, os.WNOHANG)
                if pid:
                    child = 0
                    if not os.WIFEXITED(status) or os.WEXITSTATUS(status):
                        raise AssertionError("worker failed at channel limit")
                    print("Worker enforces 128-channel limit")
                    return
                time.sleep(0.05)
            raise AssertionError("worker did not stop at channel limit")
        if os.environ.get("WORKER_TEST_CONTROL_EOF") == "1":
            server, client = socket.socketpair()
            control.sendmsg([packet(2, 1, 11001, 50001, b"example.client1")],
                            [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [server.fileno()]))])
            server.close()
            receive(control, 5, 1)
            control.close()
            client.settimeout(15)
            if client.recv(1) != b"":
                raise AssertionError("control EOF did not close the active channel")
            client.close()
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                pid, status = os.waitpid(child, os.WNOHANG)
                if pid:
                    child = 0
                    if not os.WIFEXITED(status) or os.WEXITSTATUS(status):
                        raise AssertionError("worker failed on control EOF")
                    print("Worker closes channels on control EOF")
                    return
                time.sleep(0.05)
            raise AssertionError("worker did not stop on control EOF")
        with open("/dev/null", "rb") as invalid:
            control.sendmsg([packet(2, 99, 11099, 50099, b"example.invalid")],
                            [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [invalid.fileno()]))])
        receive(control, 6, 99)
        for channel in (1, 2):
            server, client = socket.socketpair()
            control.sendmsg([packet(2, channel, 11000 + channel, 50000 + channel,
                                    ("example.client%d" % channel).encode())],
                            [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [server.fileno()]))])
            server.close()
            receive(control, 5, channel)
            process = subprocess.Popen([java, "-cp", classpath, "WorkerClientTest", str(client.fileno()),
                                        "example.client%d" % channel, "example.client%d" % (3 - channel)],
                                       pass_fds=(client.fileno(),), stdin=subprocess.PIPE, stdout=subprocess.PIPE)
            client.close()
            clients.append(process)
            wait_line(process)
        control.send(packet(3, 1))
        receive(control, 5, 1)
        clients[0].stdin.write(b"x")
        clients[0].stdin.flush()
        if clients[0].wait(timeout=10) != 0:
            raise AssertionError("first client failed")
        clients[1].stdin.write(b"c")
        clients[1].stdin.flush()
        wait_line(clients[1], b"STILL_BOUND\n")
        clients[1].stdin.write(b"x")
        clients[1].stdin.flush()
        if clients[1].wait(timeout=10) != 0:
            raise AssertionError("second client failed")
        receive(control, 7, 2)
        server, client = socket.socketpair()
        control.sendmsg([packet(2, 4, 11004, 50004, b"example.invalidframe")],
                        [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [server.fileno()]))])
        server.close()
        receive(control, 5, 4)
        client.sendall(b"\0\0\0\0")
        receive(control, 7, 4)
        client.close()
        server, client = socket.socketpair()
        control.sendmsg([packet(2, 3, 11003, 50003, b"example.third")],
                        [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [server.fileno()]))])
        server.close()
        receive(control, 5, 3)
        control.send(packet(4))
        receive(control, 5, 0)
        client.settimeout(10)
        if client.recv(1) != b"":
            raise AssertionError("STOP did not close the active channel")
        client.close()
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            pid, status = os.waitpid(child, os.WNOHANG)
            if pid:
                child = 0
                if not os.WIFEXITED(status) or os.WEXITSTATUS(status):
                    raise AssertionError("worker failed")
                break
            time.sleep(0.05)
        if child:
            raise AssertionError("worker failed to stop")
        print("Unmodified microG worker: two isolated channels and stop passed")
    finally:
        control.close()
        for process in clients:
            if process.poll() is None:
                process.kill()
                process.wait()
        if child:
            os.kill(child, 9)
            os.waitpid(child, 0)


if __name__ == "__main__":
    main()
