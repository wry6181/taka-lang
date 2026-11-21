#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
GRAMMAR="$ROOT/zed-extension/grammars/tree-sitter-taka"
EXT="$ROOT/zed-extension"
DEST="$HOME/Library/Application Support/Zed/extensions/installed/taka"

command -v cargo >/dev/null
command -v npx >/dev/null
command -v python3 >/dev/null

rustup target add wasm32-wasip2 >/dev/null 2>&1 || true

(
  cd "$GRAMMAR"
  npx --yes tree-sitter-cli generate >/dev/null
  npx --yes tree-sitter-cli build --wasm . >/dev/null
)

cargo build --manifest-path "$EXT/Cargo.toml" --release --target wasm32-wasip2 >/dev/null

rm -rf "$DEST"
mkdir -p "$DEST/grammars" "$DEST/languages/taka"
cp "$EXT/extension.toml" "$DEST/extension.toml"
cp "$EXT/target/wasm32-wasip2/release/taka_zed.wasm" "$DEST/extension.wasm"
cp "$EXT"/languages/taka/* "$DEST/languages/taka/"
cp "$GRAMMAR/tree-sitter-taka.wasm" "$DEST/grammars/taka.wasm"

if [ -w /opt/homebrew/bin ]; then
  ln -sfn "$ROOT/tools/taka_lsp.py" /opt/homebrew/bin/taka-lsp
else
  mkdir -p "$HOME/.local/bin"
  ln -sfn "$ROOT/tools/taka_lsp.py" "$HOME/.local/bin/taka-lsp"
fi

echo "Installed Taka Zed extension to:"
echo "  $DEST"
echo "Installed taka-lsp command."
echo "Restart Zed (or reload the extension) to pick up changes."
