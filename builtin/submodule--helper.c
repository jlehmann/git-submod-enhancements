#include "builtin.h"
#include "cache.h"
#include "parse-options.h"
#include "quote.h"
#include "pathspec.h"
#include "dir.h"
#include "utf8.h"

static char *ps_matched;
static const struct cache_entry **ce_entries;
static int ce_alloc, ce_used;
static struct pathspec pathspec;
static const char *alternative_path;

static void module_list_compute(int argc, const char **argv,
				const char *prefix,
				struct pathspec *pathspec)
{
	int i;
	char *max_prefix;
	int max_prefix_len;
	parse_pathspec(pathspec, 0,
		       PATHSPEC_PREFER_FULL |
		       PATHSPEC_STRIP_SUBMODULE_SLASH_CHEAP,
		       prefix, argv);

	/* Find common prefix for all pathspec's */
	max_prefix = common_prefix(pathspec);
	max_prefix_len = max_prefix ? strlen(max_prefix) : 0;

	if (pathspec->nr)
		ps_matched = xcalloc(1, pathspec->nr);


	if (read_cache() < 0)
		die("index file corrupt");

	for (i = 0; i < active_nr; i++) {
		const struct cache_entry *ce = active_cache[i];

		if (!match_pathspec(pathspec, ce->name, ce_namelen(ce),
				    max_prefix_len, ps_matched,
				    S_ISGITLINK(ce->ce_mode) | S_ISDIR(ce->ce_mode)))
			continue;

		if (S_ISGITLINK(ce->ce_mode)) {
			ALLOC_GROW(ce_entries, ce_used + 1, ce_alloc);
			ce_entries[ce_used++] = ce;
		}
	}
}

static int module_list(int argc, const char **argv, const char *prefix)
{
	int i;
	struct string_list already_printed = STRING_LIST_INIT_NODUP;

	struct option module_list_options[] = {
		OPT_STRING(0, "prefix", &alternative_path,
			   N_("path"),
			   N_("alternative anchor for relative paths")),
		OPT_END()
	};

	static const char * const git_submodule_helper_usage[] = {
		N_("git submodule--helper module_list [--prefix=<path>] [<path>...]"),
		NULL
	};

	argc = parse_options(argc, argv, prefix, module_list_options,
			     git_submodule_helper_usage, 0);

	module_list_compute(argc, argv, alternative_path
					? alternative_path
					: prefix, &pathspec);

	if (ps_matched && report_path_error(ps_matched, &pathspec, prefix)) {
		printf("#unmatched\n");
		return 1;
	}

	for (i = 0; i < ce_used; i++) {
		const struct cache_entry *ce = ce_entries[i];

		if (string_list_has_string(&already_printed, ce->name))
			continue;

		if (ce_stage(ce)) {
			printf("%06o %s U\t", ce->ce_mode, sha1_to_hex(null_sha1));
		} else {
			printf("%06o %s %d\t", ce->ce_mode, sha1_to_hex(ce->sha1), ce_stage(ce));
		}

		utf8_fprintf(stdout, "%s\n", ce->name);

		string_list_insert(&already_printed, ce->name);
	}
	return 0;
}

int cmd_submodule__helper(int argc, const char **argv, const char *prefix)
{
	if (argc < 2)
		goto usage;

	if (!strcmp(argv[1], "module_list"))
		return module_list(argc - 1, argv + 1, prefix);

usage:
	usage("git submodule--helper module_list\n");
}
