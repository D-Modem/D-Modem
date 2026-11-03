/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    t434_test.cpp - dsp/t434.h: the BINARY-DATA message's octets for a
 *                    small file, every attribute there and back (lengths
 *                    in their long forms), another coder's ways (lengths
 *                    not given, strings in pieces, EXTERNAL, attributes
 *                    we skip), t434_length() as the octets come, and
 *                    damaged messages
 *
 */

#include <string.h>

#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "t434.h"

using namespace dsp;

typedef std::vector<unsigned char> Bytes;

/* a file of the name and data, nothing else */
static BftFile file(const char *name, const char *data)
{
	BftFile f;
	f.name = name;
	f.data.assign(data, data + strlen(data));
	return f;
}

/* "a.txt" of "hi": [APPLICATION 23] { SEQUENCE { [28] { BIT STRING
   version-3 }, [0] { UTF8String }, [13] 2, [30] { OCTET STRING } } } */
TEST(T434, Encodes)
{
	Bytes out;
	t434_encode(file("a.txt", "hi"), &out);
	EXPECT_EQ(out, Bytes({0x77, 0x1a, 0x30, 0x18,
			      0xbc, 0x04, 0x03, 0x02, 0x05, 0x20,
			      0xa0, 0x07, 0x0c, 0x05, 'a', '.', 't', 'x', 't',
			      0x8d, 0x01, 0x02,
			      0xbe, 0x04, 0x04, 0x02, 'h', 'i'}));
	EXPECT_EQ(t434_length(out.data(), out.size()), (long)out.size());
}

/* all we send of two files, one of 70000 octets (three-octet lengths,
   a filesize of three octets), the other empty, its size 0x80 (a
   leading 0 octet) */
TEST(T434, RoundTrip)
{
	std::mt19937 rng(1);
	BftFile a, b;
	a.name = "report.pdf";
	a.prefix = "docs/2026/";
	a.mime = "application/pdf";
	a.modified = "20261103093000Z";
	a.data.resize(70000);
	for (auto &v : a.data)
		v = (unsigned char)rng();
	b.name = "empty";
	b.size = 0x80;
	Bytes out;
	t434_encode({a, b}, &out);
	EXPECT_EQ(out[0], 0x77);
	EXPECT_EQ(out[1], 0x83);
	std::vector<BftFile> fs;
	ASSERT_EQ(t434_decode(out.data(), out.size(), &fs), 0);
	ASSERT_EQ(fs.size(), 2u);
	EXPECT_EQ(fs[0].name, a.name);
	EXPECT_EQ(fs[0].prefix, a.prefix);
	EXPECT_EQ(fs[0].mime, a.mime);
	EXPECT_EQ(fs[0].modified, a.modified);
	EXPECT_EQ(fs[0].size, 70000);
	EXPECT_TRUE(fs[0].data == a.data);
	EXPECT_EQ(fs[1].name, "empty");
	EXPECT_EQ(fs[1].size, 0x80);
	EXPECT_TRUE(fs[1].data.empty());
	EXPECT_TRUE(fs[1].mime.empty());
}

/* as another coder may have it: indefinite lengths, no protocol
   version, attributes we don't use ([8] creator, [21] an OBJECT
   IDENTIFIER, [17] private use with a high tag inside), the name and
   the content in pieces, mime-media-type without its SEQUENCE; and a
   second file's content as EXTERNAL octet-aligned */
TEST(T434, TakesOtherCoders)
{
	Bytes m = {
		0x77, 0x80,
		0x30, 0x80,
		0x88, 0x03, 'B', 'o', 'b',
		0xa0, 0x80, 0x2c, 0x80, 0x0c, 0x02, 'x', '.', 0x0c, 0x01, 'z', 0x00, 0x00, 0x00, 0x00,
		0x95, 0x03, 0x2b, 0x06, 0x01,
		0xb1, 0x05, 0xa0, 0x03, 0xdf, 0x7f, 0x00,
		0x8d, 0x02, 0x00, 0x05,
		0xbf, 0x20, 0x0c, 0x16, 0x0a, 't', 'e', 'x', 't', '/', 'p', 'l', 'a', 'i', 'n',
		0xbe, 0x80, 0x24, 0x80, 0x04, 0x02, 'a', 'b', 0x04, 0x03, 'c', 'd', 'e', 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00,
		0x30, 0x0d,
		0xbe, 0x0b, 0x28, 0x09, 0x06, 0x02, 0x51, 0x01, 0x81, 0x03, '1', '2', '3',
		0x00, 0x00,
	};
	std::vector<BftFile> fs;
	ASSERT_EQ(t434_decode(m.data(), m.size(), &fs), 0);
	ASSERT_EQ(fs.size(), 2u);
	EXPECT_EQ(fs[0].name, "x.z");
	EXPECT_EQ(fs[0].mime, "text/plain");
	EXPECT_EQ(fs[0].size, 5);
	EXPECT_EQ(std::string(fs[0].data.begin(), fs[0].data.end()), "abcde");
	EXPECT_TRUE(fs[1].name.empty());
	EXPECT_EQ(std::string(fs[1].data.begin(), fs[1].data.end()), "123");
	EXPECT_EQ(t434_length(m.data(), m.size()), (long)m.size());
}

/* the length while the message comes (0 until it is whole), of two in a
   row the first, and -1 for what is no message */
TEST(T434, Length)
{
	Bytes one, two;
	t434_encode(file("a", "0123456789"), &one);
	t434_encode(file("b", "x"), &two);
	for (size_t n = 0; n < one.size(); n++)
		EXPECT_EQ(t434_length(one.data(), n), 0) << n;
	Bytes both = one;
	both.insert(both.end(), two.begin(), two.end());
	EXPECT_EQ(t434_length(both.data(), both.size()), (long)one.size());
	EXPECT_EQ(t434_length(both.data() + one.size(), two.size()), (long)two.size());
	const unsigned char junk[] = {0xff, 0xd8, 0xff, 0xe0};
	EXPECT_EQ(t434_length(junk, sizeof junk), -1);
	const unsigned char seq[] = {0x30, 0x00};
	EXPECT_EQ(t434_length(seq, sizeof seq), -1);
}

/* damaged messages: cut short, octets changed at random, lengths far
   too long; none is taken as more than it is, nothing breaks */
TEST(T434, Damaged)
{
	Bytes m;
	BftFile f = file("name.bin", "some data in the file");
	f.mime = "a/b";
	t434_encode(f, &m);
	std::vector<BftFile> fs;
	for (size_t n = 0; n < m.size(); n++)
		EXPECT_EQ(t434_decode(m.data(), n, &fs), -1) << n;
	std::mt19937 rng(9);
	for (int k = 0; k < 20000; k++) {
		Bytes d = m;
		for (int e = 0; e < 1 + k % 4; e++)
			d[rng() % d.size()] = (unsigned char)rng();
		s16 r = t434_decode(d.data(), d.size(), &fs);
		long l = t434_length(d.data(), d.size());
		EXPECT_LE(l, (long)d.size());
		if (r == 0) {
			for (const BftFile &g : fs)
				EXPECT_LE(g.data.size(), d.size());
		}
	}
	const unsigned char deep[] = {0x77, 0x84, 0x7f, 0xff, 0xff, 0xff, 0x30};
	EXPECT_EQ(t434_decode(deep, sizeof deep, &fs), -1);
	EXPECT_EQ(t434_length(deep, sizeof deep), -1);      /* (2 GB) */
	Bytes nest(2000, 0);
	for (size_t i = 0; i < nest.size(); i += 2) {
		nest[i] = 0x30;
		nest[i + 1] = 0x80;
	}
	nest[0] = 0x77;
	EXPECT_EQ(t434_decode(nest.data(), nest.size(), &fs), -1);
	EXPECT_EQ(t434_length(nest.data(), nest.size()), -1);
}
