/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    core_equiv.cpp - the core against slmodemd's old one: each script of
 *    scripts/ through core_script (two modems back to back, driven as a
 *    DTE would, script.cpp), and the transcript must be the script's
 *    .expect: the old core's, recorded from it, or where the core does
 *    better on purpose its own (CORE_EXPECT_WRITE=1 writes the .expect
 *    files there are anew, to be looked at in git diff; an empty one
 *    starts a new script off).
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

/* what a program prints; its exit status in *status */
static std::string output(const std::string &cmd, int *status)
{
	std::string out;
	char buf[4096];
	size_t n;
#ifdef _WIN32
	/* (binary, and its CR LF made LF here: the C library's text mode
	   loses a line's end that falls across its buffer's) */
	FILE *f = popen(cmd.c_str(), "rb");
#else
	FILE *f = popen(cmd.c_str(), "r");
#endif
	if (!f) {
		*status = -1;
		return out;
	}
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		out.append(buf, n);
	*status = pclose(f);
#ifdef _WIN32
	std::string lf;
	for (size_t i = 0; i < out.size(); i++)
		if (!(out[i] == '\r' && i + 1 < out.size() && out[i + 1] == '\n'))
			lf += out[i];
	out.swap(lf);
#endif
	return out;
}

/* path's contents into *s; false if it cannot be read */
static bool read_file(const std::string &path, std::string *s)
{
	FILE *f = fopen(path.c_str(), "rb");
	char buf[4096];
	size_t n;
	if (!f)
		return false;
	s->clear();
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		s->append(buf, n);
	fclose(f);
	return true;
}

/* s into path; false if it cannot be written */
static bool write_file(const std::string &path, const std::string &s)
{
	FILE *f = fopen(path.c_str(), "wb");
	bool ok = f && fwrite(s.data(), 1, s.size(), f) == s.size();
	if (f)
		ok = fclose(f) == 0 && ok;
	return ok;
}

#ifdef _WIN32
/* s's lines */
static std::vector<std::string> lines(const std::string &s)
{
	std::vector<std::string> v;
	size_t i = 0, j;
	while ((j = s.find('\n', i)) != std::string::npos) {
		v.push_back(s.substr(i, j - i));
		i = j + 1;
	}
	if (i < s.size())
		v.push_back(s.substr(i));
	return v;
}

/* an ATI11 (or AT#UD, or +TMO) line's measurement: its field and value; false if
   line is not one of them */
static bool measurement(const std::string &line, std::string *field,
			double *value)
{
	static const char *const fields[] = {
		"Estimated SNR (dB)",
		"Receive Signal Power Level (dBm0)",
		"Transmit Signal Power Level (dBm0)",
		"Near Echo Level (dBm0)",
		"Far Echo Level (dBm0)",
		"Receive Frame Error Count",
		"Carrier Frequency Offset (Hz)",
		"Timing Offset (ppm)",
	};
	/* AT#UD's levels (hexadecimal -dBm, dB) and receive frame errors:
	   as ATI11's ("Error Count" for the errors) */
	static const struct {
		const char *key, *field;
	} diag[] = {
		{ "DIAG <2A4D3263 10=", "DIAG 10" },
		{ "DIAG <2A4D3263 11=", "DIAG 11" },
		{ "DIAG <2A4D3263 12=", "DIAG 12" },
		{ "DIAG <2A4D3263 14=", "DIAG 14" },
		{ "DIAG <2A4D3263 15=", "DIAG 15" },
		{ "DIAG <2A4D3263 59=", "DIAG 59 Error Count" },
	};
	/* +TMO's (V.59's): a level in dBm0, and the receive frame errors
	   (2F0B, 2F0D by name or tag) */
	size_t brace = line.find('{');
	if (brace != std::string::npos && line.size() >= brace + 10 &&
	    line.compare(line.size() - 9, 9, "dBm0}\\r\\n") == 0) {
		char *end;
		*value = strtod(line.c_str() + brace + 1, &end);
		if (end == line.c_str() + brace + 1)
			return false;
		*field = line.substr(0, brace + 1) + "dB";
		return true;
	}
	static const char *const tmo_errors[] = {
		"framesReceivedDiscard{", "rxErrors{", "2F0B{", "2F0D{",
	};
	for (const char *k : tmo_errors) {
		size_t at = line.find(std::string(": ") + k);
		if (at == std::string::npos)
			continue;
		const char *v = line.c_str() + at + 2 + strlen(k);
		char *end;
		*value = strtod(v, &end);
		if (end == v)
			return false;
		*field = line.substr(0, at + 2 + strlen(k)) + " Error Count";
		return true;
	}
	for (const auto &d : diag) {
		size_t at = line.find(std::string(": ") + d.key);
		if (at == std::string::npos)
			continue;
		const char *v = line.c_str() + at + 2 + strlen(d.key);
		char *end;
		*value = static_cast<double>(strtoul(v, &end, 16));
		if (end == v)
			return false;
		*field = line.substr(0, at) + d.field;
		return true;
	}
	for (const char *f : fields) {
		size_t at = line.find(std::string(": ") + f);
		if (at == std::string::npos)
			continue;
		size_t v = line.find_first_not_of(' ', at + 2 + strlen(f));
		if (v == std::string::npos)
			return false;
		char *end;
		*value = strtod(line.c_str() + v, &end);
		if (end == line.c_str() + v)
			return false;
		*field = line.substr(0, v);
		return true;
	}
	return false;
}

/* Windows' maths library rounds its sines, logarithms and square roots
   a little otherwise than glibc's, musl's and Apple's: over a line's
   echoes that moves the measurements a little (a few tenths of a dB,
   and a few frame errors more or less under the echoes; the receivers'
   offsets).  got the same as want but in those, each within its
   tolerance: a dB (or Hz) within 1, a ppm within 20 (the echoes move
   the clock's estimate by a few), an error count none
   where want has none.  Everything else stays exact,
   and elsewhere all of it does. */
static bool close_enough(const std::string &want, const std::string &got)
{
	std::vector<std::string> w = lines(want), g = lines(got);
	if (w.size() != g.size())
		return false;
	for (size_t i = 0; i < w.size(); i++) {
		if (w[i] == g[i])
			continue;
		std::string wf, gf;
		double wv, gv;
		if (!measurement(w[i], &wf, &wv) ||
		    !measurement(g[i], &gf, &gv) || wf != gf)
			return false;
		if (wf.find("Error Count") != std::string::npos) {
			if ((wv == 0) != (gv == 0))
				return false;
		} else if (fabs(wv - gv) >
			   (wf.find("(ppm)") != std::string::npos ? 20.0 :
			    1.0)) {
			return false;
		}
	}
	return true;
}
#endif

/* the scripts of SCRIPTS_DIR (their names without .txt), sorted */
static std::vector<std::string> scripts()
{
	std::vector<std::string> v;
	std::error_code ec;
	for (const auto &e :
	     std::filesystem::directory_iterator(SCRIPTS_DIR, ec)) {
		std::string n = e.path().filename().string();
		if (n.size() > 4 && n.compare(n.size() - 4, 4, ".txt") == 0)
			v.push_back(n.substr(0, n.size() - 4));
	}
	std::sort(v.begin(), v.end());
	return v;
}

/* one test per script: the core's transcript the same as the script's
   .expect */
class CoreEquiv : public testing::TestWithParam<std::string> {};

/* the script's transcript from the core, against its .expect */
TEST_P(CoreEquiv, SameTranscript)
{
	std::string base = std::string(SCRIPTS_DIR) + "/" + GetParam();
	std::string s = base + ".txt", e = base + ".expect", want;
	int st;
	std::string t = output(std::string(SCRIPT) + " " + s, &st);

	ASSERT_EQ(st, 0) << "the core's run";
	ASSERT_TRUE(read_file(e, &want))
		<< e << ": none (an empty one, then CORE_EXPECT_WRITE=1)";
	if (getenv("CORE_EXPECT_WRITE")) {
		ASSERT_TRUE(write_file(e, t)) << e;
		return;
	}
#ifdef _WIN32
	if (close_enough(want, t))
		t = want;
#endif
	/* line by line, for a readable difference */
	EXPECT_EQ(want, t) << "the core against " << e;
}

INSTANTIATE_TEST_SUITE_P(Scripts, CoreEquiv, testing::ValuesIn(scripts()),
	[](const testing::TestParamInfo<std::string> &i) { return i.param; });

/* fax_dte: every until comes (in the other scripts some wait for what
   an earlier until took already) */
TEST(CoreScript, FaxDteUntilsCome)
{
	int st;
	std::string t = output(std::string(SCRIPT) + " " SCRIPTS_DIR
			       "/fax_dte.txt", &st);

	ASSERT_EQ(st, 0);
	EXPECT_NE(t.find(": yes"), std::string::npos);
	EXPECT_EQ(t.find("TIMEOUT"), std::string::npos) << t;
}
