set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME_DIR="$ROOT/game"
ELF="$GAME_DIR/SLUS_212.16"
CONFIG="$ROOT/recomp/sc3.toml"
GENERATED="$ROOT/generated"
TOOLS_BUILD="$ROOT/build/tools"
GAME_BUILD="$ROOT/build/game"
JOBS="${JOBS:-$(nproc)}"
