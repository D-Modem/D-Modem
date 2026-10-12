/*
 *    core_equiv.cpp - the new core against slmodemd's old one: each script
 *    of scripts/ through core_script_old and core_script_new (two modems
 *    back to back, driven as a DTE would, script.cpp), and the two
 *    transcripts must be the same
 */

#include <dirent.h>
#include <stdio.h>

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

class CoreEquiv : public testing::TestWithParam<std::string> {};

TEST_P(CoreEquiv, SameTranscript)
{
	std::string s = std::string(SCRIPTS_DIR) + "/" + GetParam() + ".txt";
	int st_old, st_new;
	std::string t_old = output(std::string(SCRIPT_OLD) + " " + s, &st_old);
	std::string t_new = output(std::string(SCRIPT_NEW) + " " + s, &st_new);

	ASSERT_EQ(st_old, 0) << "the old core's run";
	ASSERT_EQ(st_new, 0) << "the new core's run";
	EXPECT_FALSE(t_old.empty());
	/* line by line, for a readable difference */
	EXPECT_EQ(t_old, t_new);
}

INSTANTIATE_TEST_SUITE_P(Scripts, CoreEquiv, testing::ValuesIn(scripts()),
	[](const testing::TestParamInfo<std::string> &i) { return i.param; });
