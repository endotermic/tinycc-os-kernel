/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * gperf.c - the subset of GNU gperf that NetSurf's build runs (libhubbub's element-type.gperf
 * and libsvgtiny's colors.gperf), as a host tool: GNU gperf is C++ and the host has none.
 *
 * Usage: gperf [--output-file=FILE] INPUT.gperf
 *
 * Reads a keyword file with a declarations part ("%..." options, a "%{ ... %}" code block
 * and the "struct NAME;" line), "%%", the keyword lines ("keyword, initializer of the
 * other fields") and optionally a second "%%" followed by code copied verbatim.  It writes C
 * with what gperf writes and those two libraries use:
 *   - the code block, verbatim;
 *   - "static const struct NAME wordlist[]", the keywords sorted (case-folded with
 *     %ignore-case) as { "keyword", initializer } (gperf's %struct-type layout);
 *   - "const struct NAME *LOOKUP(const char *str, size_t len)": the entry whose keyword is
 *     exactly the len bytes at str (compared without case with %ignore-case), or NULL;
 *     LOOKUP is %define lookup-function-name (default in_word_set).
 * A binary search replaces gperf's perfect hash: the result is the same and the tables have
 * about 150 entries.  The other options of the two files (%language=ANSI-C, %readonly-tables,
 * %global-table, %compare-strncmp, %switch, %define hash-function-name) change only how gperf
 * builds its hash; they are accepted and need nothing here.  Any other option is an error, so
 * that a new input this tool does not understand fails the build instead of compiling wrong.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct entry { char *key; char *rest; };

static int ignore_case;

static void die(const char *msg, const char *arg)
{
	fprintf(stderr, "gperf: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
	exit(1);
}

static char *trim(char *s)
{
	char *e;
	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
		*--e = 0;
	return s;
}

static int keycmp(const char *a, const char *b)
{
	for (;; a++, b++) {
		int ca = (unsigned char)*a, cb = (unsigned char)*b;
		if (ignore_case) {
			ca = tolower(ca);
			cb = tolower(cb);
		}
		if (ca != cb || !ca)
			return ca - cb;
	}
}

static int entcmp(const void *a, const void *b)
{
	return keycmp(((const struct entry *)a)->key, ((const struct entry *)b)->key);
}

int main(int argc, char **argv)
{
	const char *out_name = NULL, *in_name = NULL;
	char lookup[256] = "in_word_set", strct[256] = "";
	char line[4096];
	char *code = NULL, *tail = NULL;
	size_t code_len = 0, tail_len = 0;
	struct entry *ents = NULL;
	size_t n = 0, cap = 0, i;
	int part = 0, in_code = 0;
	FILE *in, *out;

	for (i = 1; i < (size_t)argc; i++) {
		if (!strncmp(argv[i], "--output-file=", 14))
			out_name = argv[i] + 14;
		else if (argv[i][0] == '-')
			die("unsupported option", argv[i]);
		else if (!in_name)
			in_name = argv[i];
		else
			die("more than one input file", argv[i]);
	}
	if (!in_name)
		die("usage: gperf [--output-file=FILE] INPUT.gperf", NULL);
	if (!(in = fopen(in_name, "r")))
		die("cannot open", in_name);

	while (fgets(line, sizeof(line), in)) {
		size_t len = strlen(line);
		if (len == sizeof(line) - 1 && line[len - 1] != '\n')
			die("line too long in", in_name);
		if (part < 2 && !strncmp(line, "%%", 2)) {
			part++;
			continue;
		}
		if (part == 0) {
			char *t;
			if (in_code) {
				if (!strncmp(line, "%}", 2)) {
					in_code = 0;
					continue;
				}
				code = realloc(code, code_len + len + 1);
				memcpy(code + code_len, line, len + 1);
				code_len += len;
				continue;
			}
			if (!strncmp(line, "%{", 2)) {
				in_code = 1;
				continue;
			}
			t = trim(line);
			if (!*t)
				continue;
			if (!strncmp(t, "/*", 2) || !strncmp(t, " *", 2) || *t == '*') {
				/* the licence comment before the options */
				continue;
			}
			if (!strcmp(t, "%ignore-case")) {
				ignore_case = 1;
			} else if (!strncmp(t, "%define lookup-function-name", 28)) {
				snprintf(lookup, sizeof(lookup), "%s", trim(t + 28));
			} else if (!strncmp(t, "%define hash-function-name", 26) ||
				   !strcmp(t, "%language=ANSI-C") || !strcmp(t, "%readonly-tables") ||
				   !strcmp(t, "%global-table") || !strcmp(t, "%compare-strncmp") ||
				   !strcmp(t, "%struct-type") || !strncmp(t, "%switch=", 8)) {
				/* hash construction only: nothing to do */
			} else if (!strncmp(t, "struct ", 7) && t[strlen(t) - 1] == ';') {
				t[strlen(t) - 1] = 0;
				snprintf(strct, sizeof(strct), "%s", trim(t + 7));
			} else {
				die("unsupported declaration", t);
			}
		} else if (part == 1) {
			char *t = trim(line), *comma;
			if (!*t || *t == '#')
				continue;
			if (!(comma = strchr(t, ',')))
				die("keyword line without a comma", t);
			*comma = 0;
			if (n == cap) {
				cap = cap ? 2 * cap : 256;
				ents = realloc(ents, cap * sizeof(*ents));
			}
			ents[n].key = strdup(trim(t));
			ents[n].rest = strdup(trim(comma + 1));
			n++;
		} else {
			tail = realloc(tail, tail_len + len + 1);
			memcpy(tail + tail_len, line, len + 1);
			tail_len += len;
		}
	}
	fclose(in);
	if (part < 1 || !*strct || !n)
		die("no struct declaration or no keywords in", in_name);
	qsort(ents, n, sizeof(*ents), entcmp);
	for (i = 1; i < n; i++)
		if (!keycmp(ents[i - 1].key, ents[i].key))
			die("duplicate keyword", ents[i].key);

	if (!(out = out_name ? fopen(out_name, "w") : stdout))
		die("cannot write", out_name);
	fprintf(out, "/* generated by tools/gperf (a subset of GNU gperf) from %s */\n", in_name);
	if (code)
		fputs(code, out);
	fprintf(out, "\nstatic const struct %s wordlist[] = {\n", strct);
	for (i = 0; i < n; i++) {
		const char *k;
		fputs("\t{ \"", out);
		for (k = ents[i].key; *k; k++) {
			if (*k == '"' || *k == '\\')
				fputc('\\', out);
			fputc(*k, out);
		}
		fprintf(out, "\", %s },\n", ents[i].rest);
	}
	fputs("};\n\n", out);
	fprintf(out,
		"const struct %s *\n%s(const char *str, size_t len)\n{\n"
		"\tsize_t lo = 0, hi = sizeof(wordlist) / sizeof(wordlist[0]);\n"
		"\twhile (lo < hi) {\n"
		"\t\tsize_t mid = lo + (hi - lo) / 2, i;\n"
		"\t\tconst char *k = wordlist[mid].name;\n"
		"\t\tint c = 0;\n"
		"\t\tfor (i = 0; i < len && !c; i++) {\n"
		"\t\t\tint a = (unsigned char)str[i], b = (unsigned char)k[i];\n",
		strct, lookup);
	if (ignore_case)
		fputs("\t\t\tif (a >= 'A' && a <= 'Z') a += 'a' - 'A';\n"
		      "\t\t\tif (b >= 'A' && b <= 'Z') b += 'a' - 'A';\n", out);
	fputs("\t\t\tc = b ? a - b : 1;\n"
	      "\t\t}\n"
	      "\t\tif (!c && k[len])\n"
	      "\t\t\tc = -1;\n"
	      "\t\tif (!c)\n"
	      "\t\t\treturn &wordlist[mid];\n"
	      "\t\tif (c < 0)\n"
	      "\t\t\thi = mid;\n"
	      "\t\telse\n"
	      "\t\t\tlo = mid + 1;\n"
	      "\t}\n"
	      "\treturn NULL;\n"
	      "}\n", out);
	if (tail)
		fputs(tail, out);
	if (out != stdout && fclose(out))
		die("write error on", out_name);
	return 0;
}
