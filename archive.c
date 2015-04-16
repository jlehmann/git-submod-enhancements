#include "cache.h"
#include "commit.h"
#include "tree-walk.h"
#include "attr.h"
#include "archive.h"
#include "parse-options.h"
#include "unpack-trees.h"
#include "dir.h"
#include "refs.h"

static char const * const archive_usage[] = {
	N_("git archive [<options>] <tree-ish> [<path>...]"),
	N_("git archive --list"),
	N_("git archive --remote <repo> [--exec <cmd>] [<options>] <tree-ish> [<path>...]"),
	N_("git archive --remote <repo> [--exec <cmd>] --list"),
	NULL
};

static const struct archiver **archivers;
static int nr_archivers;
static int alloc_archivers;
static int remote_allow_unreachable;

void register_archiver(struct archiver *ar)
{
	ALLOC_GROW(archivers, nr_archivers + 1, alloc_archivers);
	archivers[nr_archivers++] = ar;
}

static void format_subst(const struct commit *commit,
                         const char *src, size_t len,
                         struct strbuf *buf)
{
	char *to_free = NULL;
	struct strbuf fmt = STRBUF_INIT;
	struct pretty_print_context ctx = {0};
	ctx.date_mode = DATE_NORMAL;
	ctx.abbrev = DEFAULT_ABBREV;

	if (src == buf->buf)
		to_free = strbuf_detach(buf, NULL);
	for (;;) {
		const char *b, *c;

		b = memmem(src, len, "$Format:", 8);
		if (!b)
			break;
		c = memchr(b + 8, '$', (src + len) - b - 8);
		if (!c)
			break;

		strbuf_reset(&fmt);
		strbuf_add(&fmt, b + 8, c - b - 8);

		strbuf_add(buf, src, b - src);
		format_commit_message(commit, fmt.buf, buf, &ctx);
		len -= c + 1 - src;
		src  = c + 1;
	}
	strbuf_add(buf, src, len);
	strbuf_release(&fmt);
	free(to_free);
}

void *sha1_file_to_archive(const struct archiver_args *args,
			   const char *path, const unsigned char *sha1,
			   unsigned int mode, enum object_type *type,
			   unsigned long *sizep)
{
	void *buffer;
	const struct commit *commit = args->convert ? args->commit : NULL;

	path += args->baselen;
	buffer = read_sha1_file(sha1, type, sizep);
	if (buffer && S_ISREG(mode)) {
		struct strbuf buf = STRBUF_INIT;
		size_t size = 0;

		strbuf_attach(&buf, buffer, *sizep, *sizep + 1);
		convert_to_working_tree(path, buf.buf, buf.len, &buf);
		if (commit)
			format_subst(commit, buf.buf, buf.len, &buf);
		buffer = strbuf_detach(&buf, &size);
		*sizep = size;
	}

	return buffer;
}

static void setup_archive_check(struct git_attr_check *check)
{
	static struct git_attr *attr_export_ignore;
	static struct git_attr *attr_export_subst;

	if (!attr_export_ignore) {
		attr_export_ignore = git_attr("export-ignore");
		attr_export_subst = git_attr("export-subst");
	}
	check[0].attr = attr_export_ignore;
	check[1].attr = attr_export_subst;
}

static int include_repository(const char *path)
{
	struct stat st;
	const char *tmp;

	/* Return early if the path does not exist since it is OK to not
	 * checkout submodules.
	 */
	if (stat(path, &st) && errno == ENOENT)
		return 1;

	tmp = read_gitfile(path);
	if (tmp) {
		path = tmp;
		if (stat(path, &st))
			die("Unable to stat submodule gitdir %s: %s (%d)",
			    path, strerror(errno), errno);
	}

	if (!S_ISDIR(st.st_mode))
		die("Submodule gitdir %s is not a directory", path);

	if (add_alt_odb(mkpath("%s/objects", path)))
		die("submodule odb %s could not be added as an alternate",
		    path);

	return 0;
}

static int check_gitlink(struct archiver_args *args, const unsigned char *sha1,
			 const char *path)
{
	switch (args->submodules) {
	case 0:
		return 0;

	case SUBMODULES_ALL:
		/* When all submodules are requested, we try to add any
		 * checked out submodules as alternate odbs. But we don't
		 * really care whether any particular submodule is checked
		 * out or not, we are going to try to traverse it anyways.
		 */
		include_repository(mkpath("%s.git", path));
		return READ_TREE_RECURSIVE;

	case SUBMODULES_CHECKEDOUT:
		/* If a repo is checked out at the gitlink path, we want to
		 * traverse into the submodule. But we ignore the current
		 * HEAD of the checked out submodule and always uses the SHA1
		 * recorded in the gitlink entry since we want the content
		 * of the archive to match the content of the <tree-ish>
		 * specified on the command line.
		 */
		if (!include_repository(mkpath("%s.git", path)))
			return READ_TREE_RECURSIVE;
		else
			return 0;

	default:
		die("archive.c: invalid value for args->submodules: %d",
		    args->submodules);
	}
}

struct directory {
	struct directory *up;
	unsigned char sha1[20];
	int baselen, len;
	unsigned mode;
	int stage;
	char path[FLEX_ARRAY];
};

struct archiver_context {
	struct archiver_args *args;
	write_archive_entry_fn_t write_entry;
	struct directory *bottom;
};

static int write_archive_entry(const unsigned char *sha1, const char *base,
		int baselen, const char *filename, unsigned mode, int stage,
		void *context)
{
	static struct strbuf path = STRBUF_INIT;
	struct archiver_context *c = context;
	struct archiver_args *args = c->args;
	write_archive_entry_fn_t write_entry = c->write_entry;
	struct git_attr_check check[2];
	const char *path_without_prefix;
	int err;

	args->convert = 0;
	strbuf_reset(&path);
	strbuf_grow(&path, PATH_MAX);
	strbuf_add(&path, args->base, args->baselen);
	strbuf_add(&path, base, baselen);
	strbuf_addstr(&path, filename);
	if (S_ISDIR(mode) || S_ISGITLINK(mode))
		strbuf_addch(&path, '/');
	path_without_prefix = path.buf + args->baselen;

	setup_archive_check(check);
	if (!git_check_attr(path_without_prefix, ARRAY_SIZE(check), check)) {
		if (ATTR_TRUE(check[0].value))
			return 0;
		args->convert = ATTR_TRUE(check[1].value);
	}

	if (S_ISDIR(mode) || S_ISGITLINK(mode)) {
		if (args->verbose)
			fprintf(stderr, "%.*s\n", (int)path.len, path.buf);
		err = write_entry(args, sha1, path.buf, path.len, mode);
		if (err)
			return err;
		return (S_ISDIR(mode) ? READ_TREE_RECURSIVE :
			check_gitlink(args, sha1, path.buf));
	}

	if (args->verbose)
		fprintf(stderr, "%.*s\n", (int)path.len, path.buf);
	return write_entry(args, sha1, path.buf, path.len, mode);
}

static int write_archive_entry_buf(const unsigned char *sha1, struct strbuf *base,
		const char *filename, unsigned mode, int stage,
		void *context)
{
	return write_archive_entry(sha1, base->buf, base->len,
				     filename, mode, stage, context);
}

static void queue_directory(const unsigned char *sha1,
		struct strbuf *base, const char *filename,
		unsigned mode, int stage, struct archiver_context *c)
{
	struct directory *d;
	d = xmallocz(sizeof(*d) + base->len + 1 + strlen(filename));
	d->up	   = c->bottom;
	d->baselen = base->len;
	d->mode	   = mode;
	d->stage   = stage;
	c->bottom  = d;
	d->len = sprintf(d->path, "%.*s%s/", (int)base->len, base->buf, filename);
	hashcpy(d->sha1, sha1);
}

static int write_directory(struct archiver_context *c)
{
	struct directory *d = c->bottom;
	int ret;

	if (!d)
		return 0;
	c->bottom = d->up;
	d->path[d->len - 1] = '\0'; /* no trailing slash */
	ret =
		write_directory(c) ||
		write_archive_entry(d->sha1, d->path, d->baselen,
				    d->path + d->baselen, d->mode,
				    d->stage, c) != READ_TREE_RECURSIVE;
	free(d);
	return ret ? -1 : 0;
}

static int queue_or_write_archive_entry(const unsigned char *sha1,
		struct strbuf *base, const char *filename,
		unsigned mode, int stage, void *context)
{
	struct archiver_context *c = context;

	while (c->bottom &&
	       !(base->len >= c->bottom->len &&
		 !strncmp(base->buf, c->bottom->path, c->bottom->len))) {
		struct directory *next = c->bottom->up;
		free(c->bottom);
		c->bottom = next;
	}

	if (S_ISDIR(mode)) {
		queue_directory(sha1, base, filename,
				mode, stage, c);
		return READ_TREE_RECURSIVE;
	}

	if (write_directory(c))
		return -1;
	return write_archive_entry(sha1, base->buf, base->len, filename, mode,
				   stage, context);
}

int write_archive_entries(struct archiver_args *args,
		write_archive_entry_fn_t write_entry)
{
	struct archiver_context context;
	struct unpack_trees_options opts;
	struct tree_desc t;
	int err;

	if (args->baselen > 0 && args->base[args->baselen - 1] == '/') {
		size_t len = args->baselen;

		while (len > 1 && args->base[len - 2] == '/')
			len--;
		if (args->verbose)
			fprintf(stderr, "%.*s\n", (int)len, args->base);
		err = write_entry(args, args->tree->object.sha1, args->base,
				  len, 040777);
		if (err)
			return err;
	}

	memset(&context, 0, sizeof(context));
	context.args = args;
	context.write_entry = write_entry;

	/*
	 * Setup index and instruct attr to read index only
	 */
	if (!args->worktree_attributes) {
		memset(&opts, 0, sizeof(opts));
		opts.index_only = 1;
		opts.head_idx = -1;
		opts.src_index = &the_index;
		opts.dst_index = &the_index;
		opts.fn = oneway_merge;
		init_tree_desc(&t, args->tree->buffer, args->tree->size);
		if (unpack_trees(1, &t, &opts))
			return -1;
		git_attr_set_direction(GIT_ATTR_INDEX, &the_index);
	}

	err = read_tree_recursive(args->tree, "", 0, 0, &args->pathspec,
				  args->pathspec.has_wildcard ?
				  queue_or_write_archive_entry :
				  write_archive_entry_buf,
				  &context);
	if (err == READ_TREE_RECURSIVE)
		err = 0;
	while (context.bottom) {
		struct directory *next = context.bottom->up;
		free(context.bottom);
		context.bottom = next;
	}
	return err;
}

static const struct archiver *lookup_archiver(const char *name)
{
	int i;

	if (!name)
		return NULL;

	for (i = 0; i < nr_archivers; i++) {
		if (!strcmp(name, archivers[i]->name))
			return archivers[i];
	}
	return NULL;
}

static int reject_entry(const unsigned char *sha1, struct strbuf *base,
			const char *filename, unsigned mode,
			int stage, void *context)
{
	int ret = -1;
	if (S_ISDIR(mode)) {
		struct strbuf sb = STRBUF_INIT;
		strbuf_addbuf(&sb, base);
		strbuf_addstr(&sb, filename);
		if (!match_pathspec(context, sb.buf, sb.len, 0, NULL, 1))
			ret = READ_TREE_RECURSIVE;
		strbuf_release(&sb);
	}
	return ret;
}

static int path_exists(struct tree *tree, const char *path)
{
	const char *paths[] = { path, NULL };
	struct pathspec pathspec;
	int ret;

	parse_pathspec(&pathspec, 0, 0, "", paths);
	pathspec.recursive = 1;
	ret = read_tree_recursive(tree, "", 0, 0, &pathspec,
				  reject_entry, &pathspec);
	free_pathspec(&pathspec);
	return ret != 0;
}

static void parse_pathspec_arg(const char **pathspec,
		struct archiver_args *ar_args)
{
	/*
	 * must be consistent with parse_pathspec in path_exists()
	 * Also if pathspec patterns are dependent, we're in big
	 * trouble as we test each one separately
	 */
	parse_pathspec(&ar_args->pathspec, 0,
		       PATHSPEC_PREFER_FULL,
		       "", pathspec);
	ar_args->pathspec.recursive = 1;
	if (pathspec) {
		while (*pathspec) {
			if (**pathspec && !path_exists(ar_args->tree, *pathspec))
				die(_("pathspec '%s' did not match any files"), *pathspec);
			pathspec++;
		}
	}
}

static void parse_treeish_arg(const char **argv,
		struct archiver_args *ar_args, const char *prefix,
		int remote)
{
	const char *name = argv[0];
	const unsigned char *commit_sha1;
	time_t archive_time;
	struct tree *tree;
	const struct commit *commit;
	unsigned char sha1[20];

	/* Remotes are only allowed to fetch actual refs */
	if (remote && !remote_allow_unreachable) {
		char *ref = NULL;
		const char *colon = strchrnul(name, ':');
		int refnamelen = colon - name;

		if (!dwim_ref(name, refnamelen, sha1, &ref))
			die("no such ref: %.*s", refnamelen, name);
		free(ref);
	}

	if (get_sha1(name, sha1))
		die("Not a valid object name");

	commit = lookup_commit_reference_gently(sha1, 1);
	if (commit) {
		commit_sha1 = commit->object.sha1;
		archive_time = commit->date;
	} else {
		commit_sha1 = NULL;
		archive_time = time(NULL);
	}

	tree = parse_tree_indirect(sha1);
	if (tree == NULL)
		die("not a tree object");

	if (prefix) {
		unsigned char tree_sha1[20];
		unsigned int mode;
		int err;

		err = get_tree_entry(tree->object.sha1, prefix,
				     tree_sha1, &mode);
		if (err || !S_ISDIR(mode))
			die("current working directory is untracked");

		tree = parse_tree_indirect(tree_sha1);
	}
	ar_args->tree = tree;
	ar_args->commit_sha1 = commit_sha1;
	ar_args->commit = commit;
	ar_args->time = archive_time;
}

#define OPT__COMPR(s, v, h, p) \
	{ OPTION_SET_INT, (s), NULL, (v), NULL, (h), \
	  PARSE_OPT_NOARG | PARSE_OPT_NONEG, NULL, (p) }
#define OPT__COMPR_HIDDEN(s, v, p) \
	{ OPTION_SET_INT, (s), NULL, (v), NULL, "", \
	  PARSE_OPT_NOARG | PARSE_OPT_NONEG | PARSE_OPT_HIDDEN, NULL, (p) }

static int parse_archive_args(int argc, const char **argv,
		const struct archiver **ar, struct archiver_args *args,
		const char *name_hint, int is_remote)
{
	const char *format = NULL;
	const char *base = NULL;
	const char *remote = NULL;
	const char *exec = NULL;
	const char *output = NULL;
	const char *submodules = NULL;
	int compression_level = -1;
	int verbose = 0;
	int i;
	int list = 0;
	int worktree_attributes = 0;
	struct option opts[] = {
		OPT_GROUP(""),
		OPT_STRING(0, "format", &format, N_("fmt"), N_("archive format")),
		OPT_STRING(0, "prefix", &base, N_("prefix"),
			N_("prepend prefix to each pathname in the archive")),
		OPT_STRING('o', "output", &output, N_("file"),
			N_("write the archive to this file")),
		OPT_BOOL(0, "worktree-attributes", &worktree_attributes,
			N_("read .gitattributes in working directory")),
		OPT__VERBOSE(&verbose, N_("report archived files on stderr")),
		{OPTION_STRING, 0, "recurse-submodules", &submodules, "kind",
			"include submodule content in the archive",
			PARSE_OPT_OPTARG, NULL, (intptr_t)"checkedout"},
		OPT__COMPR('0', &compression_level, N_("store only"), 0),
		OPT__COMPR('1', &compression_level, N_("compress faster"), 1),
		OPT__COMPR_HIDDEN('2', &compression_level, 2),
		OPT__COMPR_HIDDEN('3', &compression_level, 3),
		OPT__COMPR_HIDDEN('4', &compression_level, 4),
		OPT__COMPR_HIDDEN('5', &compression_level, 5),
		OPT__COMPR_HIDDEN('6', &compression_level, 6),
		OPT__COMPR_HIDDEN('7', &compression_level, 7),
		OPT__COMPR_HIDDEN('8', &compression_level, 8),
		OPT__COMPR('9', &compression_level, N_("compress better"), 9),
		OPT_GROUP(""),
		OPT_BOOL('l', "list", &list,
			N_("list supported archive formats")),
		OPT_GROUP(""),
		OPT_STRING(0, "remote", &remote, N_("repo"),
			N_("retrieve the archive from remote repository <repo>")),
		OPT_STRING(0, "exec", &exec, N_("command"),
			N_("path to the remote git-upload-archive command")),
		OPT_END()
	};

	argc = parse_options(argc, argv, NULL, opts, archive_usage, 0);

	if (remote)
		die("Unexpected option --remote");
	if (exec)
		die("Option --exec can only be used together with --remote");
	if (output)
		die("Unexpected option --output");

	if (!base)
		base = "";

	if (list) {
		for (i = 0; i < nr_archivers; i++)
			if (!is_remote || archivers[i]->flags & ARCHIVER_REMOTE)
				printf("%s\n", archivers[i]->name);
		exit(0);
	}

	if (!format && name_hint)
		format = archive_format_from_filename(name_hint);
	if (!format)
		format = "tar";

	/* We need at least one parameter -- tree-ish */
	if (argc < 1)
		usage_with_options(archive_usage, opts);
	*ar = lookup_archiver(format);
	if (!*ar || (is_remote && !((*ar)->flags & ARCHIVER_REMOTE)))
		die("Unknown archive format '%s'", format);

	args->compression_level = Z_DEFAULT_COMPRESSION;
	if (compression_level != -1) {
		if ((*ar)->flags & ARCHIVER_WANT_COMPRESSION_LEVELS)
			args->compression_level = compression_level;
		else {
			die("Argument not supported for format '%s': -%d",
					format, compression_level);
		}
	}

	if (!submodules)
		args->submodules = 0;
	else if (!strcmp(submodules, "checkedout"))
		args->submodules = SUBMODULES_CHECKEDOUT;
	else if (!strcmp(submodules, "all"))
		args->submodules = SUBMODULES_ALL;
	else
		die("Invalid submodule kind: %s", submodules);
	args->verbose = verbose;
	args->base = base;
	args->baselen = strlen(base);
	args->worktree_attributes = worktree_attributes;

	return argc;
}

int write_archive(int argc, const char **argv, const char *prefix,
		  int setup_prefix, const char *name_hint, int remote)
{
	int nongit = 0;
	const struct archiver *ar = NULL;
	struct archiver_args args;

	if (setup_prefix && prefix == NULL)
		prefix = setup_git_directory_gently(&nongit);

	git_config_get_bool("uploadarchive.allowunreachable", &remote_allow_unreachable);
	git_config(git_default_config, NULL);

	init_tar_archiver();
	init_zip_archiver();

	argc = parse_archive_args(argc, argv, &ar, &args, name_hint, remote);
	if (nongit) {
		/*
		 * We know this will die() with an error, so we could just
		 * die ourselves; but its error message will be more specific
		 * than what we could write here.
		 */
		setup_git_directory();
	}

	parse_treeish_arg(argv, &args, prefix, remote);
	parse_pathspec_arg(argv + 1, &args);

	return ar->write_archive(ar, &args);
}

static int match_extension(const char *filename, const char *ext)
{
	int prefixlen = strlen(filename) - strlen(ext);

	/*
	 * We need 1 character for the '.', and 1 character to ensure that the
	 * prefix is non-empty (k.e., we don't match .tar.gz with no actual
	 * filename).
	 */
	if (prefixlen < 2 || filename[prefixlen - 1] != '.')
		return 0;
	return !strcmp(filename + prefixlen, ext);
}

const char *archive_format_from_filename(const char *filename)
{
	int i;

	for (i = 0; i < nr_archivers; i++)
		if (match_extension(filename, archivers[i]->name))
			return archivers[i]->name;
	return NULL;
}
