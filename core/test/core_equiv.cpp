/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    core_equiv.cpp - the core against slmodemd's old one: each script of
 *    scripts/ through core_script (two modems back to back, driven as a
 *    DTE would, script.cpp), and the transcript must be the script's
 *    .expect: the old core's, recorded from it, or where the core does
 *    better on purpose its own (CORE_EXPECT_WRITE=1 writes the .expect
 *    files there are anew, to be looked at in git diff; an empty one
 *    starts a new script off).
 */

#include <stdio.h>
#include <stdlib.h>

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
