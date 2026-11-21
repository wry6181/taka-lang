#!/usr/bin/env python3
import json
import subprocess
import sys
import time
import queue
import threading


def send(proc, message):
    data = json.dumps(message, separators=(",", ":")).encode()
    proc.stdin.write(f"Content-Length: {len(data)}\r\n\r\n".encode())
    proc.stdin.write(data)
    proc.stdin.flush()


def reader_thread(stream, messages):
    while True:
        headers = {}
        while True:
            line = stream.readline()
            if not line:
                messages.put(None)
                return
            if line in (b"\r\n", b"\n"):
                break
            key, _, value = line.partition(b":")
            headers[key.strip().lower()] = value.strip()
        length = int(headers.get(b"content-length", b"0"))
        body = stream.read(length)
        if not body:
            messages.put(None)
            return
        messages.put(json.loads(body.decode()))


def recv(proc, timeout=5.0):
    message = proc.messages.get(timeout=timeout)
    if message is None:
        raise EOFError("LSP process closed stdout")
    return message


def wait_for(proc, predicate, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        message = recv(proc, max(0.1, deadline - time.time()))
        if predicate(message):
            return message
    raise TimeoutError("expected LSP message not received")


def main():
    if len(sys.argv) != 3:
        print("usage: lsp_test.py <taka_lsp.py> <taka>", file=sys.stderr)
        return 2
    script, taka = sys.argv[1], sys.argv[2]
    proc = subprocess.Popen(
        [sys.executable, script, "--taka", taka],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    proc.messages = queue.Queue()
    threading.Thread(target=reader_thread, args=(proc.stdout, proc.messages), daemon=True).start()
    try:
        send(proc, {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
        init = wait_for(proc, lambda m: m.get("id") == 1)
        caps = init["result"]["capabilities"]
        assert caps["textDocumentSync"]["openClose"] is True
        assert caps["semanticTokensProvider"]["full"] is True
        send(proc, {"jsonrpc": "2.0", "method": "initialized", "params": {}})

        uri = "file:///tmp/taka_lsp_test.tk"
        bad = "fn bad() { i32 x = 1; x = 2; }"
        send(
            proc,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {"textDocument": {"uri": uri, "languageId": "taka", "version": 1, "text": bad}},
            },
        )
        diagnostics = wait_for(
            proc,
            lambda m: m.get("method") == "textDocument/publishDiagnostics"
            and m["params"]["uri"] == uri,
        )
        assert diagnostics["params"]["diagnostics"], diagnostics
        assert any(d["code"] == "S0010" for d in diagnostics["params"]["diagnostics"]), diagnostics

        send(
            proc,
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": uri}},
            },
        )
        tokens = wait_for(proc, lambda m: m.get("id") == 2)
        assert tokens["result"]["data"], tokens

        good = "fn ok() <pure> -> i32 { return 1; }"
        send(
            proc,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 2},
                    "contentChanges": [{"text": good}],
                },
            },
        )
        diagnostics = wait_for(
            proc,
            lambda m: m.get("method") == "textDocument/publishDiagnostics"
            and m["params"].get("version") == 2,
        )
        assert diagnostics["params"]["diagnostics"] == [], diagnostics

        send(
            proc,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didClose",
                "params": {"textDocument": {"uri": uri}},
            },
        )
        closed = wait_for(
            proc,
            lambda m: m.get("method") == "textDocument/publishDiagnostics"
            and m["params"]["uri"] == uri,
        )
        assert closed["params"]["diagnostics"] == []

        send(proc, {"jsonrpc": "2.0", "id": 3, "method": "shutdown", "params": None})
        wait_for(proc, lambda m: m.get("id") == 3)
        send(proc, {"jsonrpc": "2.0", "method": "exit", "params": None})
        proc.wait(timeout=5)
        assert proc.returncode == 0, proc.returncode
        print("lsp test OK (initialize, diagnostics, semantic tokens, shutdown)")
        return 0
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.wait()


if __name__ == "__main__":
    raise SystemExit(main())
