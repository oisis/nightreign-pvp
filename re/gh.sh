#!/bin/sh
# Run a Ghidra script read-only against a saved project: gh.sh <er|nr> <Script.java> [args...]
. "$(dirname "$0")/common.sh"
case "$1" in er) P="$W/analysis/ghidra er-ernr"; B=eldenring.exe;; nr) P="$W/analysis/ghidra-ernr ernr"; B=nightreign.exe;; esac
S=$2; shift 2
JAVA_HOME=/usr/lib/jvm/java-21-openjdk PATH=/usr/lib/jvm/java-21-openjdk/bin:$PATH GHIDRA_HEADLESS_MAXMEM=8G \
  "$W/tools/ghidra-12.1.2/opt/ghidra/support/analyzeHeadless" $P -process $B -noanalysis -readOnly \
  -scriptPath "$R/re" -postScript "$S" "$@" 2>&1
