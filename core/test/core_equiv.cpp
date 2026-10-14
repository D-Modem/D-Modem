/*
 *    core_equiv.cpp - the new core against slmodemd's old one: each script
 *    of scripts/ through core_script_old and core_script_new (two modems
 *    back to back, driven as a DTE would, script.cpp), and the two
 *    transcripts must be the same.  Where the new core does better on
 *    purpose, scripts/<name>.expect holds its transcript, and the new
 *    core's must be that one instead (CORE_EXPECT_WRITE=1 writes the
 *    .expect files there are anew, to be looked at in git diff; an empty
 *    one starts a new script off).
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

/* what a program prints; its exit status in *status */
static std::string output(const std::string &cmd, int *status)
{
	std::string out;
	char buf[4096];
	size_t n;
	FILE *f = popen(cmd.c_str(), "r");
	if (!f) {
		*status = -1;
		return out;
	}
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		out.append(buf, n);
	*status = pclose(f);
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
	DIR *d = opendir(SCRIPTS_DIR);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		std::string n = e->d_name;
		if (n.size() > 4 && n.compare(n.size() - 4, 4, ".txt") == 0)
			v.push_back(n.substr(0, n.size() - 4));
	}
	if (d)
		closedir(d);
	std::sort(v.begin(), v.end());
	return v;
}

/* one test per script: the new core's transcript the same as the old
   core's, or as the script's .expect */
class CoreEquiv : public testing::TestWithParam<std::string> {};

TEST_P(CoreEquiv, SameTranscript)
{
	std::string base = std::string(SCRIPTS_DIR) + "/" + GetParam();
	std::string s = base + ".txt", e = base + ".expect", want;
	int st_old, st_new;
	std::string t_new = output(std::string(SCRIPT_NEW) + " " + s, &st_new);

	ASSERT_EQ(st_new, 0) << "the new core's run";
	if (read_file(e, &want)) {
		if (getenv("CORE_EXPECT_WRITE")) {
			ASSERT_TRUE(write_file(e, t_new)) << e;
			return;
		}
		EXPECT_EQ(want, t_new) << "the new core against " << e;
		return;
	}
	std::string t_old = output(std::string(SCRIPT_OLD) + " " + s, &st_old);
	ASSERT_EQ(st_old, 0) << "the old core's run";
	EXPECT_FALSE(t_old.empty());
	/* line by line, for a readable difference */
	EXPECT_EQ(t_old, t_new);
}

INSTANTIATE_TEST_SUITE_P(Scripts, CoreEquiv, testing::ValuesIn(scripts()),
	[](const testing::TestParamInfo<std::string> &i) { return i.param; });

/* fax_dte: every until comes (on the new core; in the other scripts
   some wait for what an earlier until took already) */
TEST(CoreScript, FaxDteUntilsCome)
{
	int st;
	std::string t = output(std::string(SCRIPT_NEW) + " " SCRIPTS_DIR
			       "/fax_dte.txt", &st);

	ASSERT_EQ(st, 0);
	EXPECT_NE(t.find(": yes"), std::string::npos);
	EXPECT_EQ(t.find("TIMEOUT"), std::string::npos) << t;
}
