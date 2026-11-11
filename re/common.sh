# Sourced by the project's shell scripts. Sets:
#   R  the repository root
#   W  the private work folder holding everything that is not in the repository: tools/ (Ghidra,
#      esdtool, zig, me3, wine prefix), analysis/ (game binaries and extracted game files), build/, dist/.
#      Defaults to the repository's parent folder; override with NRPVP_WORK.
R=$(cd "$(dirname "$0")/.." && pwd)
W=${NRPVP_WORK:-$(cd "$R/.." && pwd)}
# Windows-style path for programs run under wine.
w() { echo "Z:$(echo "$1" | tr / '\\')"; }
wine_run() { WINEPREFIX="$W/tools/wineprefix" WINEDEBUG=-all wine "$@"; }
