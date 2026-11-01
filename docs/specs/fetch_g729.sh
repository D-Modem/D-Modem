#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# Downloads G.729's software package from ITU-T and takes its Annex A
# test vectors (only those: data, not code) into g729a/, where
# link/test/g729a_test.cpp checks D-Modem's G.729 Annex A against them.
# ITU publishes the package free of charge but does not allow it to be
# redistributed, so the vectors are not part of the repository
# (.gitignore).
#
#   docs/specs/fetch_g729.sh
cd "$(dirname "$0")"
if [ -s g729a/SPEECH.BIT ]; then
	echo "g729a: already there"
	exit 0
fi
id='T-REC-G.729-201206-I!!SOFT-ZST-E'
url="https://www.itu.int/rec/dologin_pub.asp?lang=e&id=$id&type=items"
zip=g729-soft.zip.part
for try in 1 2 3; do
	curl -fsSL -o "$zip" "$url" && head -c 2 "$zip" | grep -q PK && break
	rm -f "$zip"
	sleep 2
done
if [ ! -s "$zip" ]; then
	echo "G.729: download of $id failed" >&2
	exit 1
fi
mkdir -p g729a
unzip -j -o -q "$zip" 'Software/G729_Release3/g729AnnexA/test_vectors/*' \
	-d g729a
rm -f "$zip"
echo "g729a: $(ls g729a | wc -l) files from $id"
