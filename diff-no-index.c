/*
 * "diff --no-index" support
 * Copyright (c) 2007 by Johannes Schindelin
 * Copyright (c) 2008 by Junio C Hamano
 */

#include "cache.h"
#include "color.h"
#include "commit.h"
#include "blob.h"
#include "tag.h"
#include "diff.h"
#include "diffcore.h"
#include "revision.h"
#include "log-tree.h"
#include "builtin.h"
#include "string-list.h"

static int read_directory(const char *path, struct string_list *list)
{
	DIR *dir;
	struct dirent *e;

	if (!(dir = opendir(path)))
		return error("Could not open directory %s", path);

	while ((e = readdir(dir)))
		if (strcmp(".", e->d_name) && strcmp("..", e->d_name))
			string_list_insert(list, e->d_name);

	closedir(dir);
	return 0;
}

/*
 * This should be "(standard input)" or something, but it will
 * probably expose many more breakages in the way no-index code
 * is bolted onto the diff callchain.
 */
static const char file_from_standard_input[] = "-";

static int get_mode(const char *path, int *mode)
{
	struct stat st;

	if (!path || !strcmp(path, "/dev/null"))
		*mode = 0;
#ifdef GIT_WINDOWS_NATIVE
	else if (!strcasecmp(path, "nul"))
		*mode = 0;
#endif
	else if (path == file_from_standard_input)
		*mode = create_ce_mode(0666);
	else if (lstat(path, &st))
		return error("Could not access '%s'", path);
	else
		*mode = st.st_mode;
	return 0;
}

static int populate_from_stdin(struct diff_filespec *s)
{
	struct strbuf buf = STRBUF_INIT;
	size_t size = 0;

	if (strbuf_read(&buf, 0, 0) < 0)
		return error("error while reading from stdin %s",
				     strerror(errno));

	s->should_munmap = 0;
	s->data = strbuf_detach(&buf, &size);
	s->size = size;
	s->should_free = 1;
	s->is_stdin = 1;
	return 0;
}

static struct diff_filespec *noindex_filespec(const char *name, int mode)
{
	struct diff_filespec *s;

	if (!name)
		name = "/dev/null";
	s = alloc_filespec(name);
	fill_filespec(s, null_sha1, 0, mode);
	if (name == file_from_standard_input)
		populate_from_stdin(s);
	return s;
}

static int queue_diff(struct diff_options *o,
		      const char *name1, const char *name2)
{
	int mode1 = 0, mode2 = 0;

	if (get_mode(name1, &mode1) || get_mode(name2, &mode2))
		return -1;

	if (mode1 && mode2 && S_ISDIR(mode1) != S_ISDIR(mode2))
		return error("file/directory conflict: %s, %s", name1, name2);

	if (S_ISDIR(mode1) || S_ISDIR(mode2)) {
		struct strbuf buffer1 = STRBUF_INIT;
		struct strbuf buffer2 = STRBUF_INIT;
		struct string_list p1 = STRING_LIST_INIT_DUP;
		struct string_list p2 = STRING_LIST_INIT_DUP;
		int i1, i2, ret = 0;
		size_t len1 = 0, len2 = 0;

		if (name1 && read_directory(name1, &p1))
			return -1;
		if (name2 && read_directory(name2, &p2)) {
			string_list_clear(&p1, 0);
			return -1;
		}

		if (name1) {
			strbuf_addstr(&buffer1, name1);
			if (buffer1.len && buffer1.buf[buffer1.len - 1] != '/')
				strbuf_addch(&buffer1, '/');
			len1 = buffer1.len;
		}

		if (name2) {
			strbuf_addstr(&buffer2, name2);
			if (buffer2.len && buffer2.buf[buffer2.len - 1] != '/')
				strbuf_addch(&buffer2, '/');
			len2 = buffer2.len;
		}

		for (i1 = i2 = 0; !ret && (i1 < p1.nr || i2 < p2.nr); ) {
			const char *n1, *n2;
			int comp;

			strbuf_setlen(&buffer1, len1);
			strbuf_setlen(&buffer2, len2);

			if (i1 == p1.nr)
				comp = 1;
			else if (i2 == p2.nr)
				comp = -1;
			else
				comp = strcmp(p1.items[i1].string, p2.items[i2].string);

			if (comp > 0)
				n1 = NULL;
			else {
				strbuf_addstr(&buffer1, p1.items[i1++].string);
				n1 = buffer1.buf;
			}

			if (comp < 0)
				n2 = NULL;
			else {
				strbuf_addstr(&buffer2, p2.items[i2++].string);
				n2 = buffer2.buf;
			}

			ret = queue_diff(o, n1, n2);
		}
		string_list_clear(&p1, 0);
		string_list_clear(&p2, 0);
		strbuf_release(&buffer1);
		strbuf_release(&buffer2);

		return ret;
	} else {
		struct diff_filespec *d1, *d2;

		if (DIFF_OPT_TST(o, REVERSE_DIFF)) {
			unsigned tmp;
			const char *tmp_c;
			tmp = mode1; mode1 = mode2; mode2 = tmp;
			tmp_c = name1; name1 = name2; name2 = tmp_c;
		}

		d1 = noindex_filespec(name1, mode1);
		d2 = noindex_filespec(name2, mode2);
		diff_queue(&diff_queued_diff, d1, d2);
		return 0;
	}
}

void diff_no_index(struct rev_info *revs,
		   int argc, const char **argv,
		   int nongit, const char *prefix)
{
	int i, prefixlen;
	int no_index = 0;
	const char *paths[2];

	/* Were we asked to do --no-index explicitly? */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--")) {
			i++;
			break;
		}
		if (!strcmp(argv[i], "--no-index"))
			no_index = 1;
		if (argv[i][0] != '-')
			break;
	}

	if (!no_index && !nongit) {
		/*
		 * Inside a git repository, without --no-index.  Only
		 * when a path outside the repository is given,
		 * e.g. "git diff /var/tmp/[12]", or "git diff
		 * Makefile /var/tmp/Makefile", allow it to be used as
		 * a colourful "diff" replacement.
		 */
		if ((argc != i + 2) ||
		    (path_inside_repo(prefix, argv[i]) &&
		     path_inside_repo(prefix, argv[i+1])))
			return;
	}
	if (argc != i + 2)
		usagef("git diff %s <path> <path>",
		       no_index ? "--no-index" : "[--no-index]");

	diff_setup(&revs->diffopt);
	for (i = 1; i < argc - 2; ) {
		int j;
		if (!strcmp(argv[i], "--no-index"))
			i++;
		else if (!strcmp(argv[i], "--"))
			i++;
		else {
			j = diff_opt_parse(&revs->diffopt, argv + i, argc - i);
			if (!j)
				die("invalid diff option/value: %s", argv[i]);
			i += j;
		}
	}

	prefixlen = prefix ? strlen(prefix) : 0;
	for (i = 0; i < 2; i++) {
		const char *p = argv[argc - 2 + i];
		if (!strcmp(p, "-"))
			/*
			 * stdin should be spelled as "-"; if you have
			 * path that is "-", spell it as "./-".
			 */
			p = file_from_standard_input;
		else if (prefixlen)
			p = xstrdup(prefix_filename(prefix, prefixlen, p));
		paths[i] = p;
	}
	revs->diffopt.skip_stat_unmatch = 1;
	if (!revs->diffopt.output_format)
		revs->diffopt.output_format = DIFF_FORMAT_PATCH;

	DIFF_OPT_SET(&revs->diffopt, NO_INDEX);

	revs->max_count = -2;
	diff_setup_done(&revs->diffopt);

	setup_diff_pager(&revs->diffopt);
	DIFF_OPT_SET(&revs->diffopt, EXIT_WITH_STATUS);

	if (queue_diff(&revs->diffopt, paths[0], paths[1]))
		exit(1);
	diff_set_mnemonic_prefix(&revs->diffopt, "1/", "2/");
	diffcore_std(&revs->diffopt);
	diff_flush(&revs->diffopt);

	/*
	 * The return code for --no-index imitates diff(1):
	 * 0 = no changes, 1 = changes, else error
	 */
	exit(diff_result_code(&revs->diffopt, 0));
}
