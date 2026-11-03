/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    t434.cpp - ITU-T T.434 binary file transfer: the BINARY-DATA
 *               message in ASN.1's basic encoding rules (X.690)
 *
 * BINARY-DATA-Message ::= [APPLICATION 23] IMPLICIT SEQUENCE OF
 * SEQUENCE { the attributes of a file }, the module's tags EXPLICIT
 * unless the attribute says IMPLICIT (T.434 Annex A).  We send of a
 * file its protocol-version [28] (version 3), filename [0], the
 * date-and-time-of-last-modification [5], filesize [13],
 * mime-media-type [32] and the data-file-content [30] as an OCTET
 * STRING; we take those (and skip the rest) in any order, with lengths
 * definite or not, and strings in pieces (constructed).
 *
 */

#include <string.h>

#include "t434.h"

namespace dsp {

namespace {

#define CLS_UNIVERSAL   0
#define CLS_APPLICATION 1
#define CLS_CONTEXT     2

#define TAG_INTEGER     2
#define TAG_BIT_STRING  3
#define TAG_OCTETS      4
#define TAG_EXTERNAL    8
#define TAG_UTF8        12
#define TAG_SEQUENCE    16
#define TAG_IA5         22

#define BFT_APPLICATION 23      /* BINARY-DATA-Message */
#define A_FILENAME      0
#define A_MODIFIED      5
#define A_FILESIZE      13
#define A_VERSION       28
#define A_CONTENT       30
#define A_MIME          32

#define MAX_DEPTH       32      /* elements in elements, at most */

/* an element's identifier and length */
struct Tlv {
	s16 cls;
	bool cons;
	u32 tag;
	size_t hdr;                     /* octets before the contents */
	size_t len;                     /* of the contents (without an
					   indefinite length's end) */
};

/* the element at p (n octets there): its size, 0 when it goes on past
   them, -1 when it is none */
long element(const unsigned char *p, size_t n, Tlv *t, s16 depth)
{
	if (depth > MAX_DEPTH)
		return -1;
	if (n < 1)
		return 0;
	size_t i = 1;
	t->cls = p[0] >> 6;
	t->cons = (p[0] & 0x20) != 0;
	t->tag = p[0] & 0x1f;
	if (t->tag == 0x1f) {
		/* the high tag number form: base 128, the last octet's top
		   bit 0 */
		t->tag = 0;
		do {
			if (i >= n)
				return 0;
			if (t->tag >> 24)
				return -1;
			t->tag = t->tag << 7 | (p[i] & 0x7f);
		} while (p[i++] & 0x80);
	}
	if (i >= n)
		return 0;
	unsigned char l = p[i++];
	if (l == 0x80) {
		/* indefinite: the elements in it, then 00 00 */
		if (!t->cons)
			return -1;
		size_t pos = i;
		for (;;) {
			if (pos + 2 > n)
				return 0;
			if (p[pos] == 0 && p[pos + 1] == 0)
				break;
			Tlv c;
			long s = element(p + pos, n - pos, &c, depth + 1);
			if (s <= 0)
				return s;
			pos += (size_t)s;
		}
		t->hdr = i;
		t->len = pos - i;
		return (long)(pos + 2);
	}
	size_t len = l;
	if (l & 0x80) {
		s16 k = l & 0x7f;
		if (k > 4)
			return -1;
		if (i + k > n)
			return 0;
		len = 0;
		while (k--)
			len = len << 8 | p[i++];
	}
	if (len > 0x7fffffff - i)
		return -1;
	if (i + len > n)
		return 0;
	t->hdr = i;
	t->len = len;
	return (long)(i + len);
}

/* f(t, contents) for each element in len octets; false when one is
   broken */
template <class F> bool each(const unsigned char *p, size_t len, s16 depth, F f)
{
	size_t pos = 0;
	while (pos < len) {
		Tlv t;
		long s = element(p + pos, len - pos, &t, depth);
		if (s <= 0)
			return false;
		if (!f(t, p + pos + t.hdr))
			return false;
		pos += (size_t)s;
	}
	return true;
}

/* a string's octets (constructed: its pieces in turn) appended */
bool string_of(const Tlv &t, const unsigned char *c, s16 depth, std::vector<unsigned char> *out)
{
	if (!t.cons) {
		out->insert(out->end(), c, c + t.len);
		return true;
	}
	return each(c, t.len, depth + 1, [&](const Tlv &s, const unsigned char *sc) {
		return string_of(s, sc, depth + 1, out);
	});
}

bool text_of(const Tlv &t, const unsigned char *c, s16 depth, std::string *out)
{
	std::vector<unsigned char> v;
	if (!string_of(t, c, depth, &v))
		return false;
	out->assign(v.begin(), v.end());
	return true;
}

/* EXTERNAL's data: its octet-aligned [1] or arbitrary [2] (a BIT
   STRING, whole octets) */
bool external_data(const unsigned char *c, size_t len, s16 depth, std::vector<unsigned char> *out)
{
	return each(c, len, depth + 1, [&](const Tlv &t, const unsigned char *tc) {
		if (t.cls != CLS_CONTEXT)
			return true;
		if (t.tag == 1)
			return string_of(t, tc, depth + 1, out);
		if (t.tag == 2 && !t.cons && t.len >= 1) {
			out->insert(out->end(), tc + 1, tc + t.len);
			return true;
		}
		return true;
	});
}

/* one file's attributes */
bool file_of(const unsigned char *c, size_t len, BftFile *f)
{
	const s16 d = 3;
	return each(c, len, d, [&](const Tlv &t, const unsigned char *tc) {
		if (t.cls != CLS_CONTEXT)
			return true;
		switch (t.tag) {
		case A_FILENAME: {
			/* the name, then its prefix in pieces */
			s16 k = 0;
			return t.cons && each(tc, t.len, d + 1, [&](const Tlv &s, const unsigned char *sc) {
				std::string part;
				if (!text_of(s, sc, d + 1, &part))
					return false;
				if (k++ == 0)
					f->name = part;
				else
					f->prefix += part;
				return true;
			});
		}
		case A_MODIFIED:
			return text_of(t, tc, d, &f->modified);
		case A_FILESIZE: {
			if (t.cons || t.len == 0 || t.len > 8)
				return false;
			s64 v = (tc[0] & 0x80) ? -1 : 0;
			for (size_t i = 0; i < t.len; i++)
				v = (s64)((u64)v << 8 | tc[i]);
			f->size = v;
			return true;
		}
		case A_CONTENT:
			if (!t.cons)    /* (tagged as though IMPLICIT) */
				return string_of(t, tc, d, &f->data);
			return each(tc, t.len, d + 1, [&](const Tlv &s, const unsigned char *sc) {
				if (s.cls == CLS_UNIVERSAL && s.tag == TAG_EXTERNAL)
					return s.cons && external_data(sc, s.len, d + 1, &f->data);
				return string_of(s, sc, d + 1, &f->data);
			});
		case A_MIME:
			/* a SEQUENCE of the type and its parameters (or the
			   type alone) */
			return each(tc, t.len, d + 1, [&](const Tlv &s, const unsigned char *sc) {
				if (s.cls == CLS_UNIVERSAL && s.tag == TAG_SEQUENCE && s.cons) {
					bool first = true;
					return each(sc, s.len, d + 2, [&](const Tlv &m, const unsigned char *mc) {
						if (first && m.cls == CLS_UNIVERSAL && m.tag == TAG_IA5) {
							first = false;
							return text_of(m, mc, d + 2, &f->mime);
						}
						first = false;
						return true;
					});
				}
				if (s.cls == CLS_UNIVERSAL && s.tag == TAG_IA5 && f->mime.empty())
					return text_of(s, sc, d + 1, &f->mime);
				return true;
			});
		default:
			return true;    /* (the attributes we don't use) */
		}
	});
}

/* an identifier (its class, constructed, tag) appended */
void put_id(std::vector<unsigned char> *out, s16 cls, bool cons, u32 tag)
{
	unsigned char b = (unsigned char)(cls << 6 | (cons ? 0x20 : 0));
	if (tag < 31) {
		out->push_back((unsigned char)(b | tag));
		return;
	}
	out->push_back(b | 0x1f);
	unsigned char v[5];
	s16 n = 0;
	do {
		v[n++] = (unsigned char)(tag & 0x7f);
		tag >>= 7;
	} while (tag);
	while (n--)
		out->push_back((unsigned char)(v[n] | (n ? 0x80 : 0)));
}

/* a definite length appended */
void put_len(std::vector<unsigned char> *out, size_t len)
{
	if (len < 0x80) {
		out->push_back((unsigned char)len);
		return;
	}
	s16 n = 0;
	for (size_t l = len; l; l >>= 8)
		n++;
	out->push_back((unsigned char)(0x80 | n));
	while (n--)
		out->push_back((unsigned char)(len >> (8 * n)));
}

/* an element of these contents */
void put(std::vector<unsigned char> *out, s16 cls, bool cons, u32 tag, const unsigned char *c, size_t len)
{
	put_id(out, cls, cons, tag);
	put_len(out, len);
	if (len)
		out->insert(out->end(), c, c + len);
}

void put(std::vector<unsigned char> *out, s16 cls, bool cons, u32 tag, const std::vector<unsigned char> &c)
{
	put(out, cls, cons, tag, c.data(), c.size());
}

void put(std::vector<unsigned char> *out, s16 cls, bool cons, u32 tag, const std::string &s)
{
	put(out, cls, cons, tag, (const unsigned char *)s.data(), s.size());
}

/* a file's SEQUENCE of attributes */
void put_file(std::vector<unsigned char> *out, const BftFile &f)
{
	std::vector<unsigned char> a, e;
	/* protocol-version [28]: BIT STRING { version-3 (2) } */
	static const unsigned char v3[] = {5, 0x20};
	put(&e, CLS_UNIVERSAL, false, TAG_BIT_STRING, v3, sizeof v3);
	put(&a, CLS_CONTEXT, true, A_VERSION, e);
	if (!f.name.empty()) {
		e.clear();
		put(&e, CLS_UNIVERSAL, false, TAG_UTF8, f.name);
		if (!f.prefix.empty())
			put(&e, CLS_UNIVERSAL, false, TAG_UTF8, f.prefix);
		put(&a, CLS_CONTEXT, true, A_FILENAME, e);
	}
	if (!f.modified.empty())
		put(&a, CLS_CONTEXT, false, A_MODIFIED, f.modified);
	/* filesize [13] IMPLICIT INTEGER: the octets, the fewest */
	u64 size = f.size >= 0 ? (u64)f.size : f.data.size();
	unsigned char iv[9];
	s16 n = 0;
	do {
		iv[n++] = (unsigned char)size;
		size >>= 8;
	} while (size);
	if (iv[n - 1] & 0x80)
		iv[n++] = 0;
	e.assign(iv, iv + n);
	std::vector<unsigned char> be(e.rbegin(), e.rend());
	put(&a, CLS_CONTEXT, false, A_FILESIZE, be);
	if (!f.mime.empty()) {
		std::vector<unsigned char> s;
		e.clear();
		put(&s, CLS_UNIVERSAL, false, TAG_IA5, f.mime);
		put(&e, CLS_UNIVERSAL, true, TAG_SEQUENCE, s);
		put(&a, CLS_CONTEXT, true, A_MIME, e);
	}
	/* data-file-content [30]: the OCTET STRING of the CHOICE */
	e.clear();
	put(&e, CLS_UNIVERSAL, false, TAG_OCTETS, f.data);
	put(&a, CLS_CONTEXT, true, A_CONTENT, e);
	put(out, CLS_UNIVERSAL, true, TAG_SEQUENCE, a);
}

} // namespace

void t434_encode(const std::vector<BftFile> &files, std::vector<unsigned char> *out)
{
	std::vector<unsigned char> seq;
	for (const BftFile &f : files)
		put_file(&seq, f);
	put(out, CLS_APPLICATION, true, BFT_APPLICATION, seq);
}

void t434_encode(const BftFile &file, std::vector<unsigned char> *out)
{
	t434_encode(std::vector<BftFile>{file}, out);
}

long t434_length(const unsigned char *data, size_t len)
{
	Tlv t;
	long s = element(data, len, &t, 0);
	if (s > 0 && (t.cls != CLS_APPLICATION || !t.cons || t.tag != BFT_APPLICATION))
		return -1;
	if (s == 0 && len >= 1 && data[0] != (0x60 | BFT_APPLICATION))
		return -1;
	return s;
}

s16 t434_decode(const unsigned char *data, size_t len, std::vector<BftFile> *files)
{
	Tlv t;
	files->clear();
	long s = element(data, len, &t, 0);
	if (s <= 0 || t.cls != CLS_APPLICATION || !t.cons || t.tag != BFT_APPLICATION)
		return -1;
	bool ok = each(data + t.hdr, t.len, 1, [&](const Tlv &f, const unsigned char *fc) {
		if (f.cls != CLS_UNIVERSAL || f.tag != TAG_SEQUENCE || !f.cons)
			return false;
		BftFile file;
		if (!file_of(fc, f.len, &file))
			return false;
		files->push_back(std::move(file));
		return true;
	});
	if (!ok) {
		files->clear();
		return -1;
	}
	return 0;
}

} // namespace dsp
