#!/bin/sh
# build.sh: docs/at_commands.pdf from at_commands.tex (pdflatex through
# latexmk; Debian's texlive-latex-recommended, texlive-latex-extra and
# latexmk).  The intermediate files go to <build>/docs/at (DMODEM_BUILD,
# default <repo>/build).
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${DMODEM_BUILD:-$here/../../build}/docs/at
mkdir -p "$out"
if ! latexmk -g -pdf -interaction=nonstopmode -halt-on-error -outdir="$out" \
	"$here/at_commands.tex" > "$out/build.log" 2>&1; then
	tail -40 "$out/build.log"
	exit 1
fi
cp "$out/at_commands.pdf" "$here/../at_commands.pdf"
echo "docs/at_commands.pdf"
