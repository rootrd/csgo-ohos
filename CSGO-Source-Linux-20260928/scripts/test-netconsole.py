#!/usr/bin/env python3
"""Run commands, open a terminal or test a password-protected Source netconsole."""

import argparse
import os
import secrets
import selectors
import socket
import sys
import time


def receive_until(connection, marker, timeout=5):
    received = b""
    deadline = time.monotonic() + timeout
    while marker not in received:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise AssertionError(f"Timed out waiting for {marker!r}\n{received[-4096:].decode(errors='replace')}")
        connection.settimeout(remaining)
        try:
            data = connection.recv(65536)
        except socket.timeout as error:
            raise AssertionError(f"Timed out waiting for {marker!r}\n{received[-4096:].decode(errors='replace')}") from error
        if not data:
            raise AssertionError("Netconsole disconnected before replying\n" + received[-4096:].decode(errors="replace"))
        received += data
    return received


def connect_authenticated(address, password, timeout):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        connection = None
        try:
            connection = socket.create_connection(address, timeout=min(2, max(0.01, deadline - time.monotonic())))
            marker = ("NETCONSOLE_READY_" + secrets.token_hex(8)).encode()
            connection.sendall(b"PASS " + password + b"\necho " + marker + b"\n")
            receive_until(connection, marker, min(2, max(0, deadline - time.monotonic())))
            return connection
        except (OSError, AssertionError) as error:
            last_error = error
            if connection is not None:
                connection.close()
            time.sleep(0.2)
    raise RuntimeError(f"Netconsole did not become ready: {last_error}. Check the game with the diagnose action.")


def run_commands(connection, commands, timeout):
    if any("\n" in command or "\r" in command for command in commands):
        raise ValueError("Pass each console command as a separate argument, without newlines")
    if any(len(command.encode()) >= 2048 for command in commands):
        raise ValueError("A console command must fit in Source's 2048-byte input buffer")
    marker = ("NETCONSOLE_DONE_" + secrets.token_hex(8)).encode()
    payload = b"".join(command.encode() + b"\n" for command in commands)
    connection.sendall(payload + b"echo " + marker + b"\n")
    reply = receive_until(connection, marker, timeout)
    print(reply.replace(marker, b"").decode(errors="replace").strip(), flush=True)


def interactive(connection):
    print("Source netconsole; Ctrl-D exits the terminal. Commands execute in the running game.", flush=True)
    with selectors.DefaultSelector() as selector:
        selector.register(connection, selectors.EVENT_READ)
        selector.register(sys.stdin, selectors.EVENT_READ)
        while True:
            for key, _ in selector.select():
                if key.fileobj is connection:
                    data = connection.recv(65536)
                    if not data:
                        raise RuntimeError("The game closed the netconsole connection")
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
                else:
                    command = sys.stdin.readline()
                    if not command:
                        return
                    connection.sendall(command.encode())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=27991)
    parser.add_argument("--password", required=True)
    parser.add_argument("--command", action="append", default=[])
    parser.add_argument("--interactive", action="store_true")
    parser.add_argument("--timeout", type=float, default=5)
    args = parser.parse_args()
    if os.environ.get("CONTAINER_ID") != "dev":
        parser.error("Run inside distrobox enter -n dev")
    if any(char in args.password for char in "\r\n"):
        parser.error("The password must fit on one line")
    if args.timeout <= 0:
        parser.error("The timeout must be positive")
    if args.command and args.interactive:
        parser.error("Use --command or --interactive, not both")

    address = ("127.0.0.1", args.port)
    password = args.password.encode()
    if args.command or args.interactive:
        with connect_authenticated(address, password, args.timeout) as connection:
            if args.command:
                run_commands(connection, args.command, args.timeout)
            elif sys.stdin.isatty():
                interactive(connection)
            else:
                run_commands(connection, [line.rstrip("\r\n") for line in sys.stdin], args.timeout)
        return
    prompt = b"Must send PASS command"
    with socket.create_connection(address, timeout=5) as connection:
        connection.sendall(b"echo NETCONSOLE_UNAUTHORIZED\n")
        reply = receive_until(connection, prompt)
        assert b"NETCONSOLE_UNAUTHORIZED" not in reply

        connection.sendall(b"PASS invalid-" + password + b"\necho NETCONSOLE_BAD_PASSWORD\n")
        reply = receive_until(connection, prompt)
        assert b"NETCONSOLE_BAD_PASSWORD" not in reply

        connection.sendall(b"PASS " + password + b"\r\necho NETCONSOLE_AUTHORIZED\n")
        receive_until(connection, b"NETCONSOLE_AUTHORIZED")

        # Leave a command unfinished across multiple engine frames.
        connection.sendall(b"echo NETCONSOLE_FRAG")
        time.sleep(0.1)
        connection.settimeout(0.25)
        try:
            partial = connection.recv(65536)
            assert b"NETCONSOLE_FRAG" not in partial
        except socket.timeout:
            pass
        connection.sendall(b"MENTED\r\n")
        receive_until(connection, b"NETCONSOLE_FRAGMENTED")

        # Several recv() buffers, with each command below Source's line limit.
        markers = [f"NETCONSOLE_BATCH_{index:03d}".encode() for index in range(64)]
        connection.sendall(b"".join(b"echo " + marker + b"\n" for marker in markers))
        reply = receive_until(connection, markers[-1])
        assert all(marker in reply for marker in markers), "A batched command was lost"

    # EOF must not poison the read loop or the next accepted connection.
    for index in range(3):
        with socket.create_connection(address, timeout=5) as connection:
            marker = f"NETCONSOLE_RECONNECT_{index}".encode()
            connection.sendall(b"PASS " + password + b"\necho " + marker + b"\n")
            receive_until(connection, marker)

    print("PASS: authentication, fragmented input, batched input, EOF and reconnect")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, AssertionError, ValueError) as error:
        sys.exit(f"[netconsole] {error}")
    except KeyboardInterrupt:
        sys.exit(130)
