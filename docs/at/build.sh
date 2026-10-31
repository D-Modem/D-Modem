#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# build.sh: docs/at_commands.pdf (A5 pages) from at_commands.tex, and
# docs/at_commands-booklet.pdf (those pages two to an A4 sheet, in
# booklet order) from at_commands_booklet.tex; pdflatex through latexmk
# (Debian's texlive-latex-recommended, texlive-latex-extra,
# texlive-fonts-recommended, texlive-pictures, lmodern and latexmk).  The
# intermediate files go to <build>/docs/at (DMODEM_BUILD, default
# <repo>/build).
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${DMODEM_BUILD:-$here/../../build}/docs/at
mkdir -p "$out"
# the version on the title page: CMakeLists.txt's project() version,
ver=$(sed -n 's/^project(dmodem VERSION \([0-9.]*\).*/\1/p' \
	"$here/../../CMakeLists.txt")
# and its date: the month of the last commit to change the manual's
# source (today's month outside a git checkout)
date=$(git -C "$here" log -1 --format=%cd --date=format:'%B %Y' \
	-- at_commands.tex 2>/dev/null || true)
[ -n "$date" ] || date=$(LC_ALL=C date +'%B %Y')
{
	printf '\\newcommand{\\dmversion}{%s}\n' "${ver:-unknown}"
	printf '\\newcommand{\\dmdate}{%s}\n' "$date"
} > "$out/dmversion.tex"
# make name: latexmk on name.tex, its log's end on a failure
make() {
	if ! TEXINPUTS="$out:" latexmk -g -pdf -interaction=nonstopmode \
		-halt-on-error -outdir="$out" "$here/$1.tex" \
		> "$out/$1-build.log" 2>&1; then
		tail -40 "$out/$1-build.log"
		exit 1
	fi
}
make at_commands
make at_commands_booklet
cp "$out/at_commands.pdf" "$here/../at_commands.pdf"
cp "$out/at_commands_booklet.pdf" "$here/../at_commands-booklet.pdf"
echo "docs/at_commands.pdf docs/at_commands-booklet.pdf"
