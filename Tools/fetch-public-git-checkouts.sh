#!/usr/bin/env bash
# Downloads only public, pinned source repositories. Never reads twilight.iso.
set -euo pipefail

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly DESTINATION="$ROOT/Upstream/git"

checkout() {
  local name="$1"
  local remote="$2"
  local revision="$3"
  local path="$DESTINATION/$name"
  if [[ -e "$path" ]]; then
    printf 'Refusing to overwrite existing checkout: %s\n' "$path" >&2
    return 1
  fi
  git clone --filter=blob:none --no-checkout "$remote" "$path"
  git -C "$path" checkout --detach "$revision"
  printf 'OK %s %s\n' "$name" "$(git -C "$path" rev-parse HEAD)"
}

mkdir -p "$DESTINATION"
checkout melee-3ds https://github.com/2gifts/melee-3ds.git 81cd9f05de43435c9cc71972c16862234c616749
checkout zeldaret-tp https://github.com/zeldaret/tp.git c8fa8c9e2aab72cf4e5db0e5d1c84a9ea6ee6eb0

printf '%s\n' 'Downloads complete. Inspect licenses before using any code.'
