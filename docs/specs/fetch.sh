#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# Downloads the ITU-T Recommendations listed in README.md into this
# directory, the latest in-force English edition of each, as PDF.
# ITU publishes them free of charge but does not allow them to be
# redistributed, so they are not part of the repository (.gitignore).
#
#   docs/specs/fetch.sh            all of them
#   docs/specs/fetch.sh V.34 T.30  some
cd "$(dirname "$0")"
RECS="G.711 T.4 T.6 T.30 T.31 T.32 T.38 V.8 V.13 V.17 V.18 V.21 V.22 V.22bis
V.23 V.25 V.27ter V.29 V.32 V.32bis V.33 V.34 V.42 V.42bis V.44 V.52 V.54 V.80
V.90 V.91 V.92 V.110 V.120 V.152 V.250 V.253 X.75"
[ $# -gt 0 ] && RECS="$*"
base=https://www.itu.int/rec
for r in $RECS; do
	if [ -s "$r.pdf" ]; then
		echo "$r.pdf: already there"
		continue
	fi
	# the newest edition in force: T-REC-<rec>-<yyyymm>-I
	# (not its amendments and appendices, T-REC-<rec>-<yyyymm>-I!Amd1)
	id=$(curl -fsSL "$base/T-REC-$r/en" |
		grep -oE "T-REC-$r-[0-9]{6}-I(![A-Za-z0-9]+)?" |
		grep -v '!' | sort -u | tail -1)
	if [ -z "$id" ]; then
		echo "$r: no edition in force found" >&2
		continue
	fi
	for try in 1 2 3; do
		curl -fsSL -o "$r.pdf.part" \
		    "$base/dologin_pub.asp?lang=e&id=$id!!PDF-E&type=items" &&
		    head -c 4 "$r.pdf.part" | grep -q '%PDF' && break
		rm -f "$r.pdf.part"
		sleep 2
	done
	if [ -s "$r.pdf.part" ]; then
		mv "$r.pdf.part" "$r.pdf"
		echo "$r.pdf: $id"
	else
		echo "$r: download of $id failed" >&2
	fi
done
