#!/bin/sh
# The block lowering's templates through bytecode.
#
# A site's template is a sealed array in the constant pool, so compiled code
# carries it as a template object (BC_TAG_TEMPLATE_OBJECT), nested arrays
# included. Compile the blocks case with qjsc, the lowering forced on, link
# it and run the binary: it must make the same checks pass as qjs does.
root=$(cd "$(dirname "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

case="$root/tests/quickts/cases/15-jsx-blocks.tsx"
if ! QJSC_JSX_BLOCKS=1 "$root/qjsc" -o "$tmp/blocks" "$case" >"$tmp/out" 2>&1; then
  echo "  FAIL  qjsc could not compile $case"
  head -5 "$tmp/out" | sed 's/^/        /'
  exit 1
fi
if out=$("$tmp/blocks" blocks 2>&1) && [ "$out" = "ok" ]; then
  echo "  ok    15-jsx-blocks.tsx compiled by qjsc, templates through bytecode"
else
  echo "  FAIL  15-jsx-blocks.tsx compiled by qjsc"
  printf '%s\n' "$out" | head -5 | sed 's/^/        /'
  exit 1
fi
