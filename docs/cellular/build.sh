#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
# build.sh: docs/v_cvm.pdf from v_cvm.tex (pdflatex through latexmk; the
# same TeX Live packages as docs/at/build.sh).  The tables of the
# measurements, measured.tex, come with the source (tables.py makes
# them anew from the measurement logs).  The intermediate files go to
# <build>/docs/cellular (DMODEM_BUILD, default <repo>/build).
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${DMODEM_BUILD:-$here/../../build}/docs/cellular
mkdir -p "$out"
if ! latexmk -g -pdf -interaction=nonstopmode -halt-on-error -outdir="$out" \
	"$here/v_cvm.tex" > "$out/build.log" 2>&1; then
	tail -40 "$out/build.log"
	exit 1
fi
cp "$out/v_cvm.pdf" "$here/../v_cvm.pdf"
echo "docs/v_cvm.pdf"
