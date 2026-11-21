#!/usr/bin/env python3
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

TOKEN_TYPES = [
    "keyword",
    "type",
    "function",
    "method",
    "variable",
    "property",
    "enumMember",
    "string",
    "number",
    "comment",
    "operator",
]
TOKEN_INDEX = {name: index for index, name in enumerate(TOKEN_TYPES)}

KEYWORDS = {
    "fn", "struct", "enum", "effect", "handler", "trait", "impl", "dyn",
    "type", "extern", "import", "mod", "pub", "private", "const", "mut",
    "auto", "where", "for", "in", "if", "else", "while", "match", "break",
    "continue", "return", "defer", "perform", "handle", "with", "resume",
    "transact", "spawn", "as", "sizeof", "offsetof", "true", "false",
    "null", "undef",
}
PRIMITIVES = {
    "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
    "f32", "f64", "char", "bool", "sview", "never",
}
EFFECTS = {
    "io", "alloc", "reads", "writes", "decides", "suspends",
    "transacts", "pure", "ownership", "gpu",
}
OPERATORS = [
    "<<=", ">>=", "::", "->", "=>", "==", "!=", "<=", ">=", "&&", "||",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>", "..",
    "+", "-", "*", "/", "%", "&", "|", "^", "!", "~", "=", "<", ">",
    "?", ":", ".", ",", ";",
]
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
NUMBER = re.compile(
    r"0[xX][0-9a-fA-F_]+|0[bB][01_]+|[0-9][0-9_]*(\.[0-9][0-9_]*)?"
    r"([eE][+-]?[0-9][0-9_]*)?[A-Za-z0-9_]*"
)
DIAG_LOC = re.compile(r"^(.*?):(\d+):(\d+): error\[([A-Z0-9]+)\]: (.*)$")
DIAG_NOLOC = re.compile(r"^(.*?): error\[([A-Z0-9]+)\]: (.*)$")


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def find_taka(explicit: str | None) -> str | None:
    candidates = []
    if explicit:
        candidates.append(explicit)
    env = os.environ.get("TAKA_BIN")
    if env:
        candidates.append(env)
    candidates.append(str(repo_root() / "build" / "taka"))
    found = shutil.which("taka")
    if found:
        candidates.append(found)
    for candidate in candidates:
        if candidate and Path(candidate).is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def uri_to_path(uri: str) -> str:
    if uri.startswith("file://"):
        from urllib.parse import unquote, urlparse

        parsed = urlparse(uri)
        return unquote(parsed.path)
    return uri


def token_at(text: str, lineno: int, column: int) -> tuple[int, int]:
    lines = text.splitlines()
    if not lines:
        return (0, 1)
    line = max(0, min(lineno, len(lines) - 1))
    current = lines[line]
    start = max(0, min(column, len(current)))
    match = IDENTIFIER.match(current, start)
    if not match:
        start = 0
        match = IDENTIFIER.search(current)
    if not match:
        return (line, start)
    return (line, match.start())


def locate_name(text: str, message: str) -> tuple[int, int, int] | None:
    lines = text.splitlines()
    for name in re.findall(r"'([^']+)'", message):
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_:]*", name):
            continue
        pattern = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"(?![A-Za-z0-9_])")
        for index, line in enumerate(lines):
            match = pattern.search(line)
            if match:
                return (index, match.start(), max(1, match.end() - match.start()))
    return None


def parse_diagnostics(text: str, output: str):
    diagnostics = []
    for raw in output.splitlines():
        match = DIAG_LOC.match(raw)
        if match:
            _, line_s, col_s, code, message = match.groups()
            line = max(0, int(line_s) - 1)
            column = max(0, int(col_s) - 1)
            length = 1
            lines = text.splitlines()
            if line < len(lines):
                token = IDENTIFIER.match(lines[line], column)
                if token:
                    length = max(1, token.end() - column)
            diagnostics.append(
                {
                    "range": {
                        "start": {"line": line, "character": column},
                        "end": {"line": line, "character": column + length},
                    },
                    "severity": 1,
                    "code": code,
                    "source": "taka",
                    "message": message,
                }
            )
            continue
        match = DIAG_NOLOC.match(raw)
        if match:
            _, code, message = match.groups()
            located = locate_name(text, message)
            if located:
                line, column, length = located
                diagnostics.append(
                    {
                        "range": {
                            "start": {"line": line, "character": column},
                            "end": {"line": line, "character": column + length},
                        },
                        "severity": 1,
                        "code": code,
                        "source": "taka",
                        "message": message,
                    }
                )
            else:
                diagnostics.append(
                    {
                        "range": {
                            "start": {"line": 0, "character": 0},
                            "end": {"line": 0, "character": 1},
                        },
                        "severity": 1,
                        "code": code,
                        "source": "taka",
                        "message": message,
                    }
                )
    return diagnostics


def semantic_tokens(text: str):
    tokens = []
    lines = text.splitlines()
    in_block = False
    block_start_line = 0
    block_start_col = 0
    block_depth = 0
    for line_index, line in enumerate(lines):
        col = 0
        if in_block:
            segment_start = 0
            while col < len(line):
                if line.startswith("/*", col):
                    block_depth += 1
                    col += 2
                    continue
                if line.startswith("*/", col):
                    block_depth -= 1
                    if block_depth == 0:
                        end = col + 2
                        tokens.append((line_index, segment_start, max(1, end - segment_start), "comment"))
                        in_block = False
                        col = end
                        segment_start = end
                        break
                    col += 2
                    continue
                col += 1
            if in_block and segment_start < len(line):
                tokens.append((line_index, segment_start, len(line) - segment_start, "comment"))
            elif in_block and not line:
                tokens.append((line_index, 0, 0, "comment"))
            if in_block:
                continue
        col = 0
        prev_nonspace = ""
        prev_word = ""
        while col < len(line):
            c = line[col]
            if c.isspace():
                col += 1
                continue
            if line.startswith("//", col):
                tokens.append((line_index, col, len(line) - col, "comment"))
                break
            if line.startswith("/*", col):
                block_start_line = line_index
                block_start_col = col
                block_depth = 1
                scan = col + 2
                ended = False
                while scan < len(line):
                    if line.startswith("/*", scan):
                        block_depth += 1
                        scan += 2
                    elif line.startswith("*/", scan):
                        block_depth -= 1
                        scan += 2
                        if block_depth == 0:
                            tokens.append((line_index, col, scan - col, "comment"))
                            ended = True
                            break
                    else:
                        scan += 1
                if ended:
                    col = scan
                    continue
                tokens.append((line_index, col, len(line) - col, "comment"))
                in_block = True
                break
            if c == '"' or c == "'":
                quote = c
                scan = col + 1
                while scan < len(line):
                    if line[scan] == "\\":
                        scan += 2
                    elif line[scan] == quote:
                        scan += 1
                        break
                    else:
                        scan += 1
                scan = min(scan, len(line))
                tokens.append((line_index, col, max(1, scan - col), "string"))
                col = scan
                prev_nonspace = quote
                continue
            match = NUMBER.match(line, col)
            if match and match.start() == col:
                tokens.append((line_index, col, match.end() - col, "number"))
                col = match.end()
                prev_nonspace = "0"
                continue
            match = IDENTIFIER.match(line, col)
            if match and match.start() == col:
                word = match.group(0)
                rest = line[match.end():]
                next_nonspace = rest.lstrip()[:1]
                prefix = line[:col].rstrip()
                after_dot = prefix.endswith(".")
                after_colon = prefix.endswith("::")
                if word in KEYWORDS or word in EFFECTS:
                    kind = "keyword"
                elif word in PRIMITIVES or word == "Self":
                    kind = "type"
                elif after_dot:
                    kind = "method" if next_nonspace == "(" else "property"
                elif after_colon:
                    kind = "function" if next_nonspace == "(" else "enumMember"
                elif next_nonspace == "(":
                    kind = "function"
                elif word[:1].isupper():
                    kind = "type"
                else:
                    kind = "variable"
                tokens.append((line_index, col, match.end() - col, kind))
                prev_word = word
                col = match.end()
                prev_nonspace = word[-1]
                continue
            matched = False
            for op in OPERATORS:
                if line.startswith(op, col):
                    tokens.append((line_index, col, len(op), "operator"))
                    col += len(op)
                    prev_nonspace = op[-1]
                    matched = True
                    break
            if matched:
                continue
            col += 1
        if in_block and not line:
            tokens.append((line_index, 0, 0, "comment"))
    encoded = []
    prev_line = 0
    prev_start = 0
    for line, start, length, kind in sorted(tokens):
        if length <= 0:
            continue
        delta_line = line - prev_line
        delta_start = start if delta_line > 0 else start - prev_start
        encoded.extend([delta_line, delta_start, length, TOKEN_INDEX[kind], 0])
        prev_line = line
        prev_start = start
    return encoded


class Server:
    def __init__(self, taka: str | None):
        self.taka = taka
        self.documents: dict[str, dict] = {}
        self.shutdown = False

    def send(self, message):
        data = json.dumps(message, separators=(",", ":")).encode("utf-8")
        sys.stdout.buffer.write(f"Content-Length: {len(data)}\r\n\r\n".encode("ascii"))
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()

    def respond(self, request_id, result):
        self.send({"jsonrpc": "2.0", "id": request_id, "result": result})

    def error(self, request_id, code, message):
        self.send({"jsonrpc": "2.0", "id": request_id, "error": {"code": code, "message": message}})

    def diagnostics(self, uri: str, text: str, version: int | None):
        diagnostics = []
        if not self.taka:
            diagnostics.append(
                {
                    "range": {
                        "start": {"line": 0, "character": 0},
                        "end": {"line": 0, "character": 1},
                    },
                    "severity": 1,
                    "code": "TOOL001",
                    "source": "taka",
                    "message": "taka binary not found; set TAKA_BIN or build the compiler",
                }
            )
        else:
            path = None
            try:
                with tempfile.NamedTemporaryFile("w", suffix=".tk", delete=False, encoding="utf-8") as handle:
                    handle.write(text)
                    path = handle.name
                result = subprocess.run(
                    [self.taka, "check", path],
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
                diagnostics = parse_diagnostics(text, result.stdout + result.stderr)
                if not diagnostics and result.returncode != 0:
                    diagnostics.append(
                        {
                            "range": {
                                "start": {"line": 0, "character": 0},
                                "end": {"line": 0, "character": 1},
                            },
                            "severity": 1,
                            "code": "TOOL002",
                            "source": "taka",
                            "message": "taka check failed without a diagnostic",
                        }
                    )
            except Exception as exc:  # noqa: BLE001
                diagnostics.append(
                    {
                        "range": {
                            "start": {"line": 0, "character": 0},
                            "end": {"line": 0, "character": 1},
                        },
                        "severity": 1,
                        "code": "TOOL003",
                        "source": "taka",
                        "message": f"taka check failed: {exc}",
                    }
                )
            finally:
                if path:
                    try:
                        os.unlink(path)
                    except OSError:
                        pass
        params = {"uri": uri, "diagnostics": diagnostics}
        if version is not None:
            params["version"] = version
        self.send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics", "params": params})

    def handle(self, message):
        method = message.get("method")
        request_id = message.get("id")
        params = message.get("params") or {}
        if method == "initialize":
            self.respond(
                request_id,
                {
                    "serverInfo": {"name": "taka-lsp", "version": "0.1.0"},
                    "capabilities": {
                        "textDocumentSync": {"openClose": True, "change": 1, "save": True},
                        "semanticTokensProvider": {
                            "legend": {"tokenTypes": TOKEN_TYPES, "tokenModifiers": []},
                            "full": True,
                        },
                    },
                },
            )
        elif method == "initialized":
            return
        elif method == "shutdown":
            self.shutdown = True
            self.respond(request_id, None)
        elif method == "exit":
            raise SystemExit(0 if self.shutdown else 1)
        elif method == "textDocument/didOpen":
            doc = params.get("textDocument") or {}
            uri = doc.get("uri", "")
            self.documents[uri] = {"text": doc.get("text", ""), "version": doc.get("version")}
            self.diagnostics(uri, self.documents[uri]["text"], self.documents[uri]["version"])
        elif method == "textDocument/didChange":
            doc = params.get("textDocument") or {}
            uri = doc.get("uri", "")
            changes = params.get("contentChanges") or []
            if changes:
                self.documents[uri] = {
                    "text": changes[-1].get("text", ""),
                    "version": doc.get("version"),
                }
                self.diagnostics(uri, self.documents[uri]["text"], self.documents[uri]["version"])
        elif method == "textDocument/didSave":
            doc = params.get("textDocument") or {}
            uri = doc.get("uri", "")
            entry = self.documents.get(uri)
            if entry:
                self.diagnostics(uri, entry["text"], entry.get("version"))
        elif method == "textDocument/didClose":
            doc = params.get("textDocument") or {}
            uri = doc.get("uri", "")
            self.documents.pop(uri, None)
            self.send(
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/publishDiagnostics",
                    "params": {"uri": uri, "diagnostics": []},
                }
            )
        elif method == "textDocument/semanticTokens/full":
            doc = params.get("textDocument") or {}
            uri = doc.get("uri", "")
            entry = self.documents.get(uri)
            data = semantic_tokens(entry["text"]) if entry else []
            self.respond(request_id, {"data": data})
        elif request_id is not None:
            self.error(request_id, -32601, f"method not found: {method}")

    def run(self):
        stdin = sys.stdin.buffer
        while True:
            headers = {}
            while True:
                line = stdin.readline()
                if not line:
                    return
                if line in (b"\r\n", b"\n"):
                    break
                key, _, value = line.partition(b":")
                headers[key.strip().lower()] = value.strip()
            length = int(headers.get(b"content-length", b"0"))
            if length <= 0:
                continue
            body = stdin.read(length)
            if not body:
                return
            try:
                message = json.loads(body.decode("utf-8"))
            except Exception:
                continue
            try:
                self.handle(message)
            except SystemExit:
                raise
            except Exception as exc:  # noqa: BLE001
                request_id = message.get("id")
                if request_id is not None:
                    self.error(request_id, -32603, str(exc))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--taka", default=None)
    parser.add_argument("--stdio", action="store_true")
    args = parser.parse_args()
    Server(find_taka(args.taka)).run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
