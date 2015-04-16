/*
 * apply.c
 *
 * Copyright (C) Linus Torvalds, 2005
 *
 * This applies patches on top of some (arbitrary) version of the SCM.
 *
 */
#include "cache.h"
#include "lockfile.h"
#include "cache-tree.h"
#include "quote.h"
#include "blob.h"
#include "delta.h"
#include "builtin.h"
#include "string-list.h"
#include "dir.h"
#include "diff.h"
#include "parse-options.h"
#include "xdiff-interface.h"
#include "ll-merge.h"
#include "rerere.h"
#include "submodule.h"

/*
 *  --check turns on checking that the working tree matches the
 *    files that are being modified, but doesn't apply the patch
 *  --stat does just a diffstat, and doesn't actually apply
 *  --numstat does numeric diffstat, and doesn't actually apply
 *  --index-info shows the old and new index info for paths if available.
 *  --index updates the cache as well.
 *  --cached updates only the cache without ever touching the working tree.
 */
static const char *prefix;
static int prefix_length = -1;
static int newfd = -1;

static int unidiff_zero;
static int p_value = 1;
static int p_value_known;
static int check_index;
static int update_index;
static int cached;
static int diffstat;
static int numstat;
static int summary;
static int check;
static int apply = 1;
static int apply_in_reverse;
static int apply_with_reject;
static int apply_verbosely;
static int allow_overlap;
static int no_add;
static int threeway;
static int unsafe_paths;
static const char *fake_ancestor;
static int line_termination = '\n';
static unsigned int p_context = UINT_MAX;
static const char *recurse_submodules_default = "off";
static int recurse_submodules = RECURSE_SUBMODULES_DEFAULT;
static const char * const apply_usage[] = {
	N_("git apply [<options>] [<patch>...]"),
	NULL
};

static enum ws_error_action {
	nowarn_ws_error,
	warn_on_ws_error,
	die_on_ws_error,
	correct_ws_error
} ws_error_action = warn_on_ws_error;
static int whitespace_error;
static int squelch_whitespace_errors = 5;
static int applied_after_fixing_ws;

static enum ws_ignore {
	ignore_ws_none,
	ignore_ws_change
} ws_ignore_action = ignore_ws_none;


static const char *patch_input_file;
static const char *root;
static int root_len;
static int read_stdin = 1;
static int options;

static void parse_whitespace_option(const char *option)
{
	if (!option) {
		ws_error_action = warn_on_ws_error;
		return;
	}
	if (!strcmp(option, "warn")) {
		ws_error_action = warn_on_ws_error;
		return;
	}
	if (!strcmp(option, "nowarn")) {
		ws_error_action = nowarn_ws_error;
		return;
	}
	if (!strcmp(option, "error")) {
		ws_error_action = die_on_ws_error;
		return;
	}
	if (!strcmp(option, "error-all")) {
		ws_error_action = die_on_ws_error;
		squelch_whitespace_errors = 0;
		return;
	}
	if (!strcmp(option, "strip") || !strcmp(option, "fix")) {
		ws_error_action = correct_ws_error;
		return;
	}
	die(_("unrecognized whitespace option '%s'"), option);
}

static void parse_ignorewhitespace_option(const char *option)
{
	if (!option || !strcmp(option, "no") ||
	    !strcmp(option, "false") || !strcmp(option, "never") ||
	    !strcmp(option, "none")) {
		ws_ignore_action = ignore_ws_none;
		return;
	}
	if (!strcmp(option, "change")) {
		ws_ignore_action = ignore_ws_change;
		return;
	}
	die(_("unrecognized whitespace ignore option '%s'"), option);
}

static void set_default_whitespace_mode(const char *whitespace_option)
{
	if (!whitespace_option && !apply_default_whitespace)
		ws_error_action = (apply ? warn_on_ws_error : nowarn_ws_error);
}

/*
 * For "diff-stat" like behaviour, we keep track of the biggest change
 * we've seen, and the longest filename. That allows us to do simple
 * scaling.
 */
static int max_change, max_len;

/*
 * Various "current state", notably line numbers and what
 * file (and how) we're patching right now.. The "is_xxxx"
 * things are flags, where -1 means "don't know yet".
 */
static int linenr = 1;

/*
 * This represents one "hunk" from a patch, starting with
 * "@@ -oldpos,oldlines +newpos,newlines @@" marker.  The
 * patch text is pointed at by patch, and its byte length
 * is stored in size.  leading and trailing are the number
 * of context lines.
 */
struct fragment {
	unsigned long leading, trailing;
	unsigned long oldpos, oldlines;
	unsigned long newpos, newlines;
	/*
	 * 'patch' is usually borrowed from buf in apply_patch(),
	 * but some codepaths store an allocated buffer.
	 */
	const char *patch;
	unsigned free_patch:1,
		rejected:1;
	int size;
	int linenr;
	struct fragment *next;
};

/*
 * When dealing with a binary patch, we reuse "leading" field
 * to store the type of the binary hunk, either deflated "delta"
 * or deflated "literal".
 */
#define binary_patch_method leading
#define BINARY_DELTA_DEFLATED	1
#define BINARY_LITERAL_DEFLATED 2

/*
 * This represents a "patch" to a file, both metainfo changes
 * such as creation/deletion, filemode and content changes represented
 * as a series of fragments.
 */
struct patch {
	char *new_name, *old_name, *def_name;
	unsigned int old_mode, new_mode;
	int is_new, is_delete;	/* -1 = unknown, 0 = false, 1 = true */
	int rejected;
	unsigned ws_rule;
	int lines_added, lines_deleted;
	int score;
	unsigned int is_toplevel_relative:1;
	unsigned int inaccurate_eof:1;
	unsigned int is_binary:1;
	unsigned int is_copy:1;
	unsigned int is_rename:1;
	unsigned int recount:1;
	unsigned int conflicted_threeway:1;
	unsigned int direct_to_threeway:1;
	struct fragment *fragments;
	char *result;
	size_t resultsize;
	char old_sha1_prefix[41];
	char new_sha1_prefix[41];
	struct patch *next;

	/* three-way fallback result */
	unsigned char threeway_stage[3][20];
};

static void free_fragment_list(struct fragment *list)
{
	while (list) {
		struct fragment *next = list->next;
		if (list->free_patch)
			free((char *)list->patch);
		free(list);
		list = next;
	}
}

static void free_patch(struct patch *patch)
{
	free_fragment_list(patch->fragments);
	free(patch->def_name);
	free(patch->old_name);
	free(patch->new_name);
	free(patch->result);
	free(patch);
}

static void free_patch_list(struct patch *list)
{
	while (list) {
		struct patch *next = list->next;
		free_patch(list);
		list = next;
	}
}

/*
 * A line in a file, len-bytes long (includes the terminating LF,
 * except for an incomplete line at the end if the file ends with
 * one), and its contents hashes to 'hash'.
 */
struct line {
	size_t len;
	unsigned hash : 24;
	unsigned flag : 8;
#define LINE_COMMON     1
#define LINE_PATCHED	2
};

/*
 * This represents a "file", which is an array of "lines".
 */
struct image {
	char *buf;
	size_t len;
	size_t nr;
	size_t alloc;
	struct line *line_allocated;
	struct line *line;
};

/*
 * Records filenames that have been touched, in order to handle
 * the case where more than one patches touch the same file.
 */

static struct string_list fn_table;

static uint32_t hash_line(const char *cp, size_t len)
{
	size_t i;
	uint32_t h;
	for (i = 0, h = 0; i < len; i++) {
		if (!isspace(cp[i])) {
			h = h * 3 + (cp[i] & 0xff);
		}
	}
	return h;
}

/*
 * Compare lines s1 of length n1 and s2 of length n2, ignoring
 * whitespace difference. Returns 1 if they match, 0 otherwise
 */
static int fuzzy_matchlines(const char *s1, size_t n1,
			    const char *s2, size_t n2)
{
	const char *last1 = s1 + n1 - 1;
	const char *last2 = s2 + n2 - 1;
	int result = 0;

	/* ignore line endings */
	while ((*last1 == '\r') || (*last1 == '\n'))
		last1--;
	while ((*last2 == '\r') || (*last2 == '\n'))
		last2--;

	/* skip leading whitespaces, if both begin with whitespace */
	if (s1 <= last1 && s2 <= last2 && isspace(*s1) && isspace(*s2)) {
		while (isspace(*s1) && (s1 <= last1))
			s1++;
		while (isspace(*s2) && (s2 <= last2))
			s2++;
	}
	/* early return if both lines are empty */
	if ((s1 > last1) && (s2 > last2))
		return 1;
	while (!result) {
		result = *s1++ - *s2++;
		/*
		 * Skip whitespace inside. We check for whitespace on
		 * both buffers because we don't want "a b" to match
		 * "ab"
		 */
		if (isspace(*s1) && isspace(*s2)) {
			while (isspace(*s1) && s1 <= last1)
				s1++;
			while (isspace(*s2) && s2 <= last2)
				s2++;
		}
		/*
		 * If we reached the end on one side only,
		 * lines don't match
		 */
		if (
		    ((s2 > last2) && (s1 <= last1)) ||
		    ((s1 > last1) && (s2 <= last2)))
			return 0;
		if ((s1 > last1) && (s2 > last2))
			break;
	}

	return !result;
}

static void add_line_info(struct image *img, const char *bol, size_t len, unsigned flag)
{
	ALLOC_GROW(img->line_allocated, img->nr + 1, img->alloc);
	img->line_allocated[img->nr].len = len;
	img->line_allocated[img->nr].hash = hash_line(bol, len);
	img->line_allocated[img->nr].flag = flag;
	img->nr++;
}

/*
 * "buf" has the file contents to be patched (read from various sources).
 * attach it to "image" and add line-based index to it.
 * "image" now owns the "buf".
 */
static void prepare_image(struct image *image, char *buf, size_t len,
			  int prepare_linetable)
{
	const char *cp, *ep;

	memset(image, 0, sizeof(*image));
	image->buf = buf;
	image->len = len;

	if (!prepare_linetable)
		return;

	ep = image->buf + image->len;
	cp = image->buf;
	while (cp < ep) {
		const char *next;
		for (next = cp; next < ep && *next != '\n'; next++)
			;
		if (next < ep)
			next++;
		add_line_info(image, cp, next - cp, 0);
		cp = next;
	}
	image->line = image->line_allocated;
}

static void clear_image(struct image *image)
{
	free(image->buf);
	free(image->line_allocated);
	memset(image, 0, sizeof(*image));
}

/* fmt must contain _one_ %s and no other substitution */
static void say_patch_name(FILE *output, const char *fmt, struct patch *patch)
{
	struct strbuf sb = STRBUF_INIT;

	if (patch->old_name && patch->new_name &&
	    strcmp(patch->old_name, patch->new_name)) {
		quote_c_style(patch->old_name, &sb, NULL, 0);
		strbuf_addstr(&sb, " => ");
		quote_c_style(patch->new_name, &sb, NULL, 0);
	} else {
		const char *n = patch->new_name;
		if (!n)
			n = patch->old_name;
		quote_c_style(n, &sb, NULL, 0);
	}
	fprintf(output, fmt, sb.buf);
	fputc('\n', output);
	strbuf_release(&sb);
}

#define SLOP (16)

static void read_patch_file(struct strbuf *sb, int fd)
{
	if (strbuf_read(sb, fd, 0) < 0)
		die_errno("git apply: failed to read");

	/*
	 * Make sure that we have some slop in the buffer
	 * so that we can do speculative "memcmp" etc, and
	 * see to it that it is NUL-filled.
	 */
	strbuf_grow(sb, SLOP);
	memset(sb->buf + sb->len, 0, SLOP);
}

static unsigned long linelen(const char *buffer, unsigned long size)
{
	unsigned long len = 0;
	while (size--) {
		len++;
		if (*buffer++ == '\n')
			break;
	}
	return len;
}

static int is_dev_null(const char *str)
{
	return skip_prefix(str, "/dev/null", &str) && isspace(*str);
}

#define TERM_SPACE	1
#define TERM_TAB	2

static int name_terminate(const char *name, int namelen, int c, int terminate)
{
	if (c == ' ' && !(terminate & TERM_SPACE))
		return 0;
	if (c == '\t' && !(terminate & TERM_TAB))
		return 0;

	return 1;
}

/* remove double slashes to make --index work with such filenames */
static char *squash_slash(char *name)
{
	int i = 0, j = 0;

	if (!name)
		return NULL;

	while (name[i]) {
		if ((name[j++] = name[i++]) == '/')
			while (name[i] == '/')
				i++;
	}
	name[j] = '\0';
	return name;
}

static char *find_name_gnu(const char *line, const char *def, int p_value)
{
	struct strbuf name = STRBUF_INIT;
	char *cp;

	/*
	 * Proposed "new-style" GNU patch/diff format; see
	 * http://marc.info/?l=git&m=112927316408690&w=2
	 */
	if (unquote_c_style(&name, line, NULL)) {
		strbuf_release(&name);
		return NULL;
	}

	for (cp = name.buf; p_value; p_value--) {
		cp = strchr(cp, '/');
		if (!cp) {
			strbuf_release(&name);
			return NULL;
		}
		cp++;
	}

	strbuf_remove(&name, 0, cp - name.buf);
	if (root)
		strbuf_insert(&name, 0, root, root_len);
	return squash_slash(strbuf_detach(&name, NULL));
}

static size_t sane_tz_len(const char *line, size_t len)
{
	const char *tz, *p;

	if (len < strlen(" +0500") || line[len-strlen(" +0500")] != ' ')
		return 0;
	tz = line + len - strlen(" +0500");

	if (tz[1] != '+' && tz[1] != '-')
		return 0;

	for (p = tz + 2; p != line + len; p++)
		if (!isdigit(*p))
			return 0;

	return line + len - tz;
}

static size_t tz_with_colon_len(const char *line, size_t len)
{
	const char *tz, *p;

	if (len < strlen(" +08:00") || line[len - strlen(":00")] != ':')
		return 0;
	tz = line + len - strlen(" +08:00");

	if (tz[0] != ' ' || (tz[1] != '+' && tz[1] != '-'))
		return 0;
	p = tz + 2;
	if (!isdigit(*p++) || !isdigit(*p++) || *p++ != ':' ||
	    !isdigit(*p++) || !isdigit(*p++))
		return 0;

	return line + len - tz;
}

static size_t date_len(const char *line, size_t len)
{
	const char *date, *p;

	if (len < strlen("72-02-05") || line[len-strlen("-05")] != '-')
		return 0;
	p = date = line + len - strlen("72-02-05");

	if (!isdigit(*p++) || !isdigit(*p++) || *p++ != '-' ||
	    !isdigit(*p++) || !isdigit(*p++) || *p++ != '-' ||
	    !isdigit(*p++) || !isdigit(*p++))	/* Not a date. */
		return 0;

	if (date - line >= strlen("19") &&
	    isdigit(date[-1]) && isdigit(date[-2]))	/* 4-digit year */
		date -= strlen("19");

	return line + len - date;
}

static size_t short_time_len(const char *line, size_t len)
{
	const char *time, *p;

	if (len < strlen(" 07:01:32") || line[len-strlen(":32")] != ':')
		return 0;
	p = time = line + len - strlen(" 07:01:32");

	/* Permit 1-digit hours? */
	if (*p++ != ' ' ||
	    !isdigit(*p++) || !isdigit(*p++) || *p++ != ':' ||
	    !isdigit(*p++) || !isdigit(*p++) || *p++ != ':' ||
	    !isdigit(*p++) || !isdigit(*p++))	/* Not a time. */
		return 0;

	return line + len - time;
}

static size_t fractional_time_len(const char *line, size_t len)
{
	const char *p;
	size_t n;

	/* Expected format: 19:41:17.620000023 */
	if (!len || !isdigit(line[len - 1]))
		return 0;
	p = line + len - 1;

	/* Fractional seconds. */
	while (p > line && isdigit(*p))
		p--;
	if (*p != '.')
		return 0;

	/* Hours, minutes, and whole seconds. */
	n = short_time_len(line, p - line);
	if (!n)
		return 0;

	return line + len - p + n;
}

static size_t trailing_spaces_len(const char *line, size_t len)
{
	const char *p;

	/* Expected format: ' ' x (1 or more)  */
	if (!len || line[len - 1] != ' ')
		return 0;

	p = line + len;
	while (p != line) {
		p--;
		if (*p != ' ')
			return line + len - (p + 1);
	}

	/* All spaces! */
	return len;
}

static size_t diff_timestamp_len(const char *line, size_t len)
{
	const char *end = line + len;
	size_t n;

	/*
	 * Posix: 2010-07-05 19:41:17
	 * GNU: 2010-07-05 19:41:17.620000023 -0500
	 */

	if (!isdigit(end[-1]))
		return 0;

	n = sane_tz_len(line, end - line);
	if (!n)
		n = tz_with_colon_len(line, end - line);
	end -= n;

	n = short_time_len(line, end - line);
	if (!n)
		n = fractional_time_len(line, end - line);
	end -= n;

	n = date_len(line, end - line);
	if (!n)	/* No date.  Too bad. */
		return 0;
	end -= n;

	if (end == line)	/* No space before date. */
		return 0;
	if (end[-1] == '\t') {	/* Success! */
		end--;
		return line + len - end;
	}
	if (end[-1] != ' ')	/* No space before date. */
		return 0;

	/* Whitespace damage. */
	end -= trailing_spaces_len(line, end - line);
	return line + len - end;
}

static char *find_name_common(const char *line, const char *def,
			      int p_value, const char *end, int terminate)
{
	int len;
	const char *start = NULL;

	if (p_value == 0)
		start = line;
	while (line != end) {
		char c = *line;

		if (!end && isspace(c)) {
			if (c == '\n')
				break;
			if (name_terminate(start, line-start, c, terminate))
				break;
		}
		line++;
		if (c == '/' && !--p_value)
			start = line;
	}
	if (!start)
		return squash_slash(xstrdup_or_null(def));
	len = line - start;
	if (!len)
		return squash_slash(xstrdup_or_null(def));

	/*
	 * Generally we prefer the shorter name, especially
	 * if the other one is just a variation of that with
	 * something else tacked on to the end (ie "file.orig"
	 * or "file~").
	 */
	if (def) {
		int deflen = strlen(def);
		if (deflen < len && !strncmp(start, def, deflen))
			return squash_slash(xstrdup(def));
	}

	if (root) {
		char *ret = xmalloc(root_len + len + 1);
		strcpy(ret, root);
		memcpy(ret + root_len, start, len);
		ret[root_len + len] = '\0';
		return squash_slash(ret);
	}

	return squash_slash(xmemdupz(start, len));
}

static char *find_name(const char *line, char *def, int p_value, int terminate)
{
	if (*line == '"') {
		char *name = find_name_gnu(line, def, p_value);
		if (name)
			return name;
	}

	return find_name_common(line, def, p_value, NULL, terminate);
}

static char *find_name_traditional(const char *line, char *def, int p_value)
{
	size_t len;
	size_t date_len;

	if (*line == '"') {
		char *name = find_name_gnu(line, def, p_value);
		if (name)
			return name;
	}

	len = strchrnul(line, '\n') - line;
	date_len = diff_timestamp_len(line, len);
	if (!date_len)
		return find_name_common(line, def, p_value, NULL, TERM_TAB);
	len -= date_len;

	return find_name_common(line, def, p_value, line + len, 0);
}

static int count_slashes(const char *cp)
{
	int cnt = 0;
	char ch;

	while ((ch = *cp++))
		if (ch == '/')
			cnt++;
	return cnt;
}

/*
 * Given the string after "--- " or "+++ ", guess the appropriate
 * p_value for the given patch.
 */
static int guess_p_value(const char *nameline)
{
	char *name, *cp;
	int val = -1;

	if (is_dev_null(nameline))
		return -1;
	name = find_name_traditional(nameline, NULL, 0);
	if (!name)
		return -1;
	cp = strchr(name, '/');
	if (!cp)
		val = 0;
	else if (prefix) {
		/*
		 * Does it begin with "a/$our-prefix" and such?  Then this is
		 * very likely to apply to our directory.
		 */
		if (!strncmp(name, prefix, prefix_length))
			val = count_slashes(prefix);
		else {
			cp++;
			if (!strncmp(cp, prefix, prefix_length))
				val = count_slashes(prefix) + 1;
		}
	}
	free(name);
	return val;
}

/*
 * Does the ---/+++ line has the POSIX timestamp after the last HT?
 * GNU diff puts epoch there to signal a creation/deletion event.  Is
 * this such a timestamp?
 */
static int has_epoch_timestamp(const char *nameline)
{
	/*
	 * We are only interested in epoch timestamp; any non-zero
	 * fraction cannot be one, hence "(\.0+)?" in the regexp below.
	 * For the same reason, the date must be either 1969-12-31 or
	 * 1970-01-01, and the seconds part must be "00".
	 */
	const char stamp_regexp[] =
		"^(1969-12-31|1970-01-01)"
		" "
		"[0-2][0-9]:[0-5][0-9]:00(\\.0+)?"
		" "
		"([-+][0-2][0-9]:?[0-5][0-9])\n";
	const char *timestamp = NULL, *cp, *colon;
	static regex_t *stamp;
	regmatch_t m[10];
	int zoneoffset;
	int hourminute;
	int status;

	for (cp = nameline; *cp != '\n'; cp++) {
		if (*cp == '\t')
			timestamp = cp + 1;
	}
	if (!timestamp)
		return 0;
	if (!stamp) {
		stamp = xmalloc(sizeof(*stamp));
		if (regcomp(stamp, stamp_regexp, REG_EXTENDED)) {
			warning(_("Cannot prepare timestamp regexp %s"),
				stamp_regexp);
			return 0;
		}
	}

	status = regexec(stamp, timestamp, ARRAY_SIZE(m), m, 0);
	if (status) {
		if (status != REG_NOMATCH)
			warning(_("regexec returned %d for input: %s"),
				status, timestamp);
		return 0;
	}

	zoneoffset = strtol(timestamp + m[3].rm_so + 1, (char **) &colon, 10);
	if (*colon == ':')
		zoneoffset = zoneoffset * 60 + strtol(colon + 1, NULL, 10);
	else
		zoneoffset = (zoneoffset / 100) * 60 + (zoneoffset % 100);
	if (timestamp[m[3].rm_so] == '-')
		zoneoffset = -zoneoffset;

	/*
	 * YYYY-MM-DD hh:mm:ss must be from either 1969-12-31
	 * (west of GMT) or 1970-01-01 (east of GMT)
	 */
	if ((zoneoffset < 0 && memcmp(timestamp, "1969-12-31", 10)) ||
	    (0 <= zoneoffset && memcmp(timestamp, "1970-01-01", 10)))
		return 0;

	hourminute = (strtol(timestamp + 11, NULL, 10) * 60 +
		      strtol(timestamp + 14, NULL, 10) -
		      zoneoffset);

	return ((zoneoffset < 0 && hourminute == 1440) ||
		(0 <= zoneoffset && !hourminute));
}

/*
 * Get the name etc info from the ---/+++ lines of a traditional patch header
 *
 * FIXME! The end-of-filename heuristics are kind of screwy. For existing
 * files, we can happily check the index for a match, but for creating a
 * new file we should try to match whatever "patch" does. I have no idea.
 */
static void parse_traditional_patch(const char *first, const char *second, struct patch *patch)
{
	char *name;

	first += 4;	/* skip "--- " */
	second += 4;	/* skip "+++ " */
	if (!p_value_known) {
		int p, q;
		p = guess_p_value(first);
		q = guess_p_value(second);
		if (p < 0) p = q;
		if (0 <= p && p == q) {
			p_value = p;
			p_value_known = 1;
		}
	}
	if (is_dev_null(first)) {
		patch->is_new = 1;
		patch->is_delete = 0;
		name = find_name_traditional(second, NULL, p_value);
		patch->new_name = name;
	} else if (is_dev_null(second)) {
		patch->is_new = 0;
		patch->is_delete = 1;
		name = find_name_traditional(first, NULL, p_value);
		patch->old_name = name;
	} else {
		char *first_name;
		first_name = find_name_traditional(first, NULL, p_value);
		name = find_name_traditional(second, first_name, p_value);
		free(first_name);
		if (has_epoch_timestamp(first)) {
			patch->is_new = 1;
			patch->is_delete = 0;
			patch->new_name = name;
		} else if (has_epoch_timestamp(second)) {
			patch->is_new = 0;
			patch->is_delete = 1;
			patch->old_name = name;
		} else {
			patch->old_name = name;
			patch->new_name = xstrdup_or_null(name);
		}
	}
	if (!name)
		die(_("unable to find filename in patch at line %d"), linenr);
}

static int gitdiff_hdrend(const char *line, struct patch *patch)
{
	return -1;
}

/*
 * We're anal about diff header consistency, to make
 * sure that we don't end up having strange ambiguous
 * patches floating around.
 *
 * As a result, gitdiff_{old|new}name() will check
 * their names against any previous information, just
 * to make sure..
 */
#define DIFF_OLD_NAME 0
#define DIFF_NEW_NAME 1

static char *gitdiff_verify_name(const char *line, int isnull, char *orig_name, int side)
{
	if (!orig_name && !isnull)
		return find_name(line, NULL, p_value, TERM_TAB);

	if (orig_name) {
		int len;
		const char *name;
		char *another;
		name = orig_name;
		len = strlen(name);
		if (isnull)
			die(_("git apply: bad git-diff - expected /dev/null, got %s on line %d"), name, linenr);
		another = find_name(line, NULL, p_value, TERM_TAB);
		if (!another || memcmp(another, name, len + 1))
			die((side == DIFF_NEW_NAME) ?
			    _("git apply: bad git-diff - inconsistent new filename on line %d") :
			    _("git apply: bad git-diff - inconsistent old filename on line %d"), linenr);
		free(another);
		return orig_name;
	}
	else {
		/* expect "/dev/null" */
		if (memcmp("/dev/null", line, 9) || line[9] != '\n')
			die(_("git apply: bad git-diff - expected /dev/null on line %d"), linenr);
		return NULL;
	}
}

static int gitdiff_oldname(const char *line, struct patch *patch)
{
	char *orig = patch->old_name;
	patch->old_name = gitdiff_verify_name(line, patch->is_new, patch->old_name,
					      DIFF_OLD_NAME);
	if (orig != patch->old_name)
		free(orig);
	return 0;
}

static int gitdiff_newname(const char *line, struct patch *patch)
{
	char *orig = patch->new_name;
	patch->new_name = gitdiff_verify_name(line, patch->is_delete, patch->new_name,
					      DIFF_NEW_NAME);
	if (orig != patch->new_name)
		free(orig);
	return 0;
}

static int gitdiff_oldmode(const char *line, struct patch *patch)
{
	patch->old_mode = strtoul(line, NULL, 8);
	return 0;
}

static int gitdiff_newmode(const char *line, struct patch *patch)
{
	patch->new_mode = strtoul(line, NULL, 8);
	return 0;
}

static int gitdiff_delete(const char *line, struct patch *patch)
{
	patch->is_delete = 1;
	free(patch->old_name);
	patch->old_name = xstrdup_or_null(patch->def_name);
	return gitdiff_oldmode(line, patch);
}

static int gitdiff_newfile(const char *line, struct patch *patch)
{
	patch->is_new = 1;
	free(patch->new_name);
	patch->new_name = xstrdup_or_null(patch->def_name);
	return gitdiff_newmode(line, patch);
}

static int gitdiff_copysrc(const char *line, struct patch *patch)
{
	patch->is_copy = 1;
	free(patch->old_name);
	patch->old_name = find_name(line, NULL, p_value ? p_value - 1 : 0, 0);
	return 0;
}

static int gitdiff_copydst(const char *line, struct patch *patch)
{
	patch->is_copy = 1;
	free(patch->new_name);
	patch->new_name = find_name(line, NULL, p_value ? p_value - 1 : 0, 0);
	return 0;
}

static int gitdiff_renamesrc(const char *line, struct patch *patch)
{
	patch->is_rename = 1;
	free(patch->old_name);
	patch->old_name = find_name(line, NULL, p_value ? p_value - 1 : 0, 0);
	return 0;
}

static int gitdiff_renamedst(const char *line, struct patch *patch)
{
	patch->is_rename = 1;
	free(patch->new_name);
	patch->new_name = find_name(line, NULL, p_value ? p_value - 1 : 0, 0);
	return 0;
}

static int gitdiff_similarity(const char *line, struct patch *patch)
{
	unsigned long val = strtoul(line, NULL, 10);
	if (val <= 100)
		patch->score = val;
	return 0;
}

static int gitdiff_dissimilarity(const char *line, struct patch *patch)
{
	unsigned long val = strtoul(line, NULL, 10);
	if (val <= 100)
		patch->score = val;
	return 0;
}

static int gitdiff_index(const char *line, struct patch *patch)
{
	/*
	 * index line is N hexadecimal, "..", N hexadecimal,
	 * and optional space with octal mode.
	 */
	const char *ptr, *eol;
	int len;

	ptr = strchr(line, '.');
	if (!ptr || ptr[1] != '.' || 40 < ptr - line)
		return 0;
	len = ptr - line;
	memcpy(patch->old_sha1_prefix, line, len);
	patch->old_sha1_prefix[len] = 0;

	line = ptr + 2;
	ptr = strchr(line, ' ');
	eol = strchrnul(line, '\n');

	if (!ptr || eol < ptr)
		ptr = eol;
	len = ptr - line;

	if (40 < len)
		return 0;
	memcpy(patch->new_sha1_prefix, line, len);
	patch->new_sha1_prefix[len] = 0;
	if (*ptr == ' ')
		patch->old_mode = strtoul(ptr+1, NULL, 8);
	return 0;
}

/*
 * This is normal for a diff that doesn't change anything: we'll fall through
 * into the next diff. Tell the parser to break out.
 */
static int gitdiff_unrecognized(const char *line, struct patch *patch)
{
	return -1;
}

/*
 * Skip p_value leading components from "line"; as we do not accept
 * absolute paths, return NULL in that case.
 */
static const char *skip_tree_prefix(const char *line, int llen)
{
	int nslash;
	int i;

	if (!p_value)
		return (llen && line[0] == '/') ? NULL : line;

	nslash = p_value;
	for (i = 0; i < llen; i++) {
		int ch = line[i];
		if (ch == '/' && --nslash <= 0)
			return (i == 0) ? NULL : &line[i + 1];
	}
	return NULL;
}

/*
 * This is to extract the same name that appears on "diff --git"
 * line.  We do not find and return anything if it is a rename
 * patch, and it is OK because we will find the name elsewhere.
 * We need to reliably find name only when it is mode-change only,
 * creation or deletion of an empty file.  In any of these cases,
 * both sides are the same name under a/ and b/ respectively.
 */
static char *git_header_name(const char *line, int llen)
{
	const char *name;
	const char *second = NULL;
	size_t len, line_len;

	line += strlen("diff --git ");
	llen -= strlen("diff --git ");

	if (*line == '"') {
		const char *cp;
		struct strbuf first = STRBUF_INIT;
		struct strbuf sp = STRBUF_INIT;

		if (unquote_c_style(&first, line, &second))
			goto free_and_fail1;

		/* strip the a/b prefix including trailing slash */
		cp = skip_tree_prefix(first.buf, first.len);
		if (!cp)
			goto free_and_fail1;
		strbuf_remove(&first, 0, cp - first.buf);

		/*
		 * second points at one past closing dq of name.
		 * find the second name.
		 */
		while ((second < line + llen) && isspace(*second))
			second++;

		if (line + llen <= second)
			goto free_and_fail1;
		if (*second == '"') {
			if (unquote_c_style(&sp, second, NULL))
				goto free_and_fail1;
			cp = skip_tree_prefix(sp.buf, sp.len);
			if (!cp)
				goto free_and_fail1;
			/* They must match, otherwise ignore */
			if (strcmp(cp, first.buf))
				goto free_and_fail1;
			strbuf_release(&sp);
			return strbuf_detach(&first, NULL);
		}

		/* unquoted second */
		cp = skip_tree_prefix(second, line + llen - second);
		if (!cp)
			goto free_and_fail1;
		if (line + llen - cp != first.len ||
		    memcmp(first.buf, cp, first.len))
			goto free_and_fail1;
		return strbuf_detach(&first, NULL);

	free_and_fail1:
		strbuf_release(&first);
		strbuf_release(&sp);
		return NULL;
	}

	/* unquoted first name */
	name = skip_tree_prefix(line, llen);
	if (!name)
		return NULL;

	/*
	 * since the first name is unquoted, a dq if exists must be
	 * the beginning of the second name.
	 */
	for (second = name; second < line + llen; second++) {
		if (*second == '"') {
			struct strbuf sp = STRBUF_INIT;
			const char *np;

			if (unquote_c_style(&sp, second, NULL))
				goto free_and_fail2;

			np = skip_tree_prefix(sp.buf, sp.len);
			if (!np)
				goto free_and_fail2;

			len = sp.buf + sp.len - np;
			if (len < second - name &&
			    !strncmp(np, name, len) &&
			    isspace(name[len])) {
				/* Good */
				strbuf_remove(&sp, 0, np - sp.buf);
				return strbuf_detach(&sp, NULL);
			}

		free_and_fail2:
			strbuf_release(&sp);
			return NULL;
		}
	}

	/*
	 * Accept a name only if it shows up twice, exactly the same
	 * form.
	 */
	second = strchr(name, '\n');
	if (!second)
		return NULL;
	line_len = second - name;
	for (len = 0 ; ; len++) {
		switch (name[len]) {
		default:
			continue;
		case '\n':
			return NULL;
		case '\t': case ' ':
			/*
			 * Is this the separator between the preimage
			 * and the postimage pathname?  Again, we are
			 * only interested in the case where there is
			 * no rename, as this is only to set def_name
			 * and a rename patch has the names elsewhere
			 * in an unambiguous form.
			 */
			if (!name[len + 1])
				return NULL; /* no postimage name */
			second = skip_tree_prefix(name + len + 1,
						  line_len - (len + 1));
			if (!second)
				return NULL;
			/*
			 * Does len bytes starting at "name" and "second"
			 * (that are separated by one HT or SP we just
			 * found) exactly match?
			 */
			if (second[len] == '\n' && !strncmp(name, second, len))
				return xmemdupz(name, len);
		}
	}
}

/* Verify that we recognize the lines following a git header */
static int parse_git_header(const char *line, int len, unsigned int size, struct patch *patch)
{
	unsigned long offset;

	/* A git diff has explicit new/delete information, so we don't guess */
	patch->is_new = 0;
	patch->is_delete = 0;

	/*
	 * Some things may not have the old name in the
	 * rest of the headers anywhere (pure mode changes,
	 * or removing or adding empty files), so we get
	 * the default name from the header.
	 */
	patch->def_name = git_header_name(line, len);
	if (patch->def_name && root) {
		char *s = xstrfmt("%s%s", root, patch->def_name);
		free(patch->def_name);
		patch->def_name = s;
	}

	line += len;
	size -= len;
	linenr++;
	for (offset = len ; size > 0 ; offset += len, size -= len, line += len, linenr++) {
		static const struct opentry {
			const char *str;
			int (*fn)(const char *, struct patch *);
		} optable[] = {
			{ "@@ -", gitdiff_hdrend },
			{ "--- ", gitdiff_oldname },
			{ "+++ ", gitdiff_newname },
			{ "old mode ", gitdiff_oldmode },
			{ "new mode ", gitdiff_newmode },
			{ "deleted file mode ", gitdiff_delete },
			{ "new file mode ", gitdiff_newfile },
			{ "copy from ", gitdiff_copysrc },
			{ "copy to ", gitdiff_copydst },
			{ "rename old ", gitdiff_renamesrc },
			{ "rename new ", gitdiff_renamedst },
			{ "rename from ", gitdiff_renamesrc },
			{ "rename to ", gitdiff_renamedst },
			{ "similarity index ", gitdiff_similarity },
			{ "dissimilarity index ", gitdiff_dissimilarity },
			{ "index ", gitdiff_index },
			{ "", gitdiff_unrecognized },
		};
		int i;

		len = linelen(line, size);
		if (!len || line[len-1] != '\n')
			break;
		for (i = 0; i < ARRAY_SIZE(optable); i++) {
			const struct opentry *p = optable + i;
			int oplen = strlen(p->str);
			if (len < oplen || memcmp(p->str, line, oplen))
				continue;
			if (p->fn(line + oplen, patch) < 0)
				return offset;
			break;
		}
	}

	return offset;
}

static int parse_num(const char *line, unsigned long *p)
{
	char *ptr;

	if (!isdigit(*line))
		return 0;
	*p = strtoul(line, &ptr, 10);
	return ptr - line;
}

static int parse_range(const char *line, int len, int offset, const char *expect,
		       unsigned long *p1, unsigned long *p2)
{
	int digits, ex;

	if (offset < 0 || offset >= len)
		return -1;
	line += offset;
	len -= offset;

	digits = parse_num(line, p1);
	if (!digits)
		return -1;

	offset += digits;
	line += digits;
	len -= digits;

	*p2 = 1;
	if (*line == ',') {
		digits = parse_num(line+1, p2);
		if (!digits)
			return -1;

		offset += digits+1;
		line += digits+1;
		len -= digits+1;
	}

	ex = strlen(expect);
	if (ex > len)
		return -1;
	if (memcmp(line, expect, ex))
		return -1;

	return offset + ex;
}

static void recount_diff(const char *line, int size, struct fragment *fragment)
{
	int oldlines = 0, newlines = 0, ret = 0;

	if (size < 1) {
		warning("recount: ignore empty hunk");
		return;
	}

	for (;;) {
		int len = linelen(line, size);
		size -= len;
		line += len;

		if (size < 1)
			break;

		switch (*line) {
		case ' ': case '\n':
			newlines++;
			/* fall through */
		case '-':
			oldlines++;
			continue;
		case '+':
			newlines++;
			continue;
		case '\\':
			continue;
		case '@':
			ret = size < 3 || !starts_with(line, "@@ ");
			break;
		case 'd':
			ret = size < 5 || !starts_with(line, "diff ");
			break;
		default:
			ret = -1;
			break;
		}
		if (ret) {
			warning(_("recount: unexpected line: %.*s"),
				(int)linelen(line, size), line);
			return;
		}
		break;
	}
	fragment->oldlines = oldlines;
	fragment->newlines = newlines;
}

/*
 * Parse a unified diff fragment header of the
 * form "@@ -a,b +c,d @@"
 */
static int parse_fragment_header(const char *line, int len, struct fragment *fragment)
{
	int offset;

	if (!len || line[len-1] != '\n')
		return -1;

	/* Figure out the number of lines in a fragment */
	offset = parse_range(line, len, 4, " +", &fragment->oldpos, &fragment->oldlines);
	offset = parse_range(line, len, offset, " @@", &fragment->newpos, &fragment->newlines);

	return offset;
}

static int find_header(const char *line, unsigned long size, int *hdrsize, struct patch *patch)
{
	unsigned long offset, len;

	patch->is_toplevel_relative = 0;
	patch->is_rename = patch->is_copy = 0;
	patch->is_new = patch->is_delete = -1;
	patch->old_mode = patch->new_mode = 0;
	patch->old_name = patch->new_name = NULL;
	for (offset = 0; size > 0; offset += len, size -= len, line += len, linenr++) {
		unsigned long nextlen;

		len = linelen(line, size);
		if (!len)
			break;

		/* Testing this early allows us to take a few shortcuts.. */
		if (len < 6)
			continue;

		/*
		 * Make sure we don't find any unconnected patch fragments.
		 * That's a sign that we didn't find a header, and that a
		 * patch has become corrupted/broken up.
		 */
		if (!memcmp("@@ -", line, 4)) {
			struct fragment dummy;
			if (parse_fragment_header(line, len, &dummy) < 0)
				continue;
			die(_("patch fragment without header at line %d: %.*s"),
			    linenr, (int)len-1, line);
		}

		if (size < len + 6)
			break;

		/*
		 * Git patch? It might not have a real patch, just a rename
		 * or mode change, so we handle that specially
		 */
		if (!memcmp("diff --git ", line, 11)) {
			int git_hdr_len = parse_git_header(line, len, size, patch);
			if (git_hdr_len <= len)
				continue;
			if (!patch->old_name && !patch->new_name) {
				if (!patch->def_name)
					die(Q_("git diff header lacks filename information when removing "
					       "%d leading pathname component (line %d)",
					       "git diff header lacks filename information when removing "
					       "%d leading pathname components (line %d)",
					       p_value),
					    p_value, linenr);
				patch->old_name = xstrdup(patch->def_name);
				patch->new_name = xstrdup(patch->def_name);
			}
			if (!patch->is_delete && !patch->new_name)
				die("git diff header lacks filename information "
				    "(line %d)", linenr);
			patch->is_toplevel_relative = 1;
			*hdrsize = git_hdr_len;
			return offset;
		}

		/* --- followed by +++ ? */
		if (memcmp("--- ", line,  4) || memcmp("+++ ", line + len, 4))
			continue;

		/*
		 * We only accept unified patches, so we want it to
		 * at least have "@@ -a,b +c,d @@\n", which is 14 chars
		 * minimum ("@@ -0,0 +1 @@\n" is the shortest).
		 */
		nextlen = linelen(line + len, size - len);
		if (size < nextlen + 14 || memcmp("@@ -", line + len + nextlen, 4))
			continue;

		/* Ok, we'll consider it a patch */
		parse_traditional_patch(line, line+len, patch);
		*hdrsize = len + nextlen;
		linenr += 2;
		return offset;
	}
	return -1;
}

static void record_ws_error(unsigned result, const char *line, int len, int linenr)
{
	char *err;

	if (!result)
		return;

	whitespace_error++;
	if (squelch_whitespace_errors &&
	    squelch_whitespace_errors < whitespace_error)
		return;

	err = whitespace_error_string(result);
	fprintf(stderr, "%s:%d: %s.\n%.*s\n",
		patch_input_file, linenr, err, len, line);
	free(err);
}

static void check_whitespace(const char *line, int len, unsigned ws_rule)
{
	unsigned result = ws_check(line + 1, len - 1, ws_rule);

	record_ws_error(result, line + 1, len - 2, linenr);
}

/*
 * Parse a unified diff. Note that this really needs to parse each
 * fragment separately, since the only way to know the difference
 * between a "---" that is part of a patch, and a "---" that starts
 * the next patch is to look at the line counts..
 */
static int parse_fragment(const char *line, unsigned long size,
			  struct patch *patch, struct fragment *fragment)
{
	int added, deleted;
	int len = linelen(line, size), offset;
	unsigned long oldlines, newlines;
	unsigned long leading, trailing;

	offset = parse_fragment_header(line, len, fragment);
	if (offset < 0)
		return -1;
	if (offset > 0 && patch->recount)
		recount_diff(line + offset, size - offset, fragment);
	oldlines = fragment->oldlines;
	newlines = fragment->newlines;
	leading = 0;
	trailing = 0;

	/* Parse the thing.. */
	line += len;
	size -= len;
	linenr++;
	added = deleted = 0;
	for (offset = len;
	     0 < size;
	     offset += len, size -= len, line += len, linenr++) {
		if (!oldlines && !newlines)
			break;
		len = linelen(line, size);
		if (!len || line[len-1] != '\n')
			return -1;
		switch (*line) {
		default:
			return -1;
		case '\n': /* newer GNU diff, an empty context line */
		case ' ':
			oldlines--;
			newlines--;
			if (!deleted && !added)
				leading++;
			trailing++;
			if (!apply_in_reverse &&
			    ws_error_action == correct_ws_error)
				check_whitespace(line, len, patch->ws_rule);
			break;
		case '-':
			if (apply_in_reverse &&
			    ws_error_action != nowarn_ws_error)
				check_whitespace(line, len, patch->ws_rule);
			deleted++;
			oldlines--;
			trailing = 0;
			break;
		case '+':
			if (!apply_in_reverse &&
			    ws_error_action != nowarn_ws_error)
				check_whitespace(line, len, patch->ws_rule);
			added++;
			newlines--;
			trailing = 0;
			break;

		/*
		 * We allow "\ No newline at end of file". Depending
                 * on locale settings when the patch was produced we
                 * don't know what this line looks like. The only
                 * thing we do know is that it begins with "\ ".
		 * Checking for 12 is just for sanity check -- any
		 * l10n of "\ No newline..." is at least that long.
		 */
		case '\\':
			if (len < 12 || memcmp(line, "\\ ", 2))
				return -1;
			break;
		}
	}
	if (oldlines || newlines)
		return -1;
	fragment->leading = leading;
	fragment->trailing = trailing;

	/*
	 * If a fragment ends with an incomplete line, we failed to include
	 * it in the above loop because we hit oldlines == newlines == 0
	 * before seeing it.
	 */
	if (12 < size && !memcmp(line, "\\ ", 2))
		offset += linelen(line, size);

	patch->lines_added += added;
	patch->lines_deleted += deleted;

	if (0 < patch->is_new && oldlines)
		return error(_("new file depends on old contents"));
	if (0 < patch->is_delete && newlines)
		return error(_("deleted file still has contents"));
	return offset;
}

/*
 * We have seen "diff --git a/... b/..." header (or a traditional patch
 * header).  Read hunks that belong to this patch into fragments and hang
 * them to the given patch structure.
 *
 * The (fragment->patch, fragment->size) pair points into the memory given
 * by the caller, not a copy, when we return.
 */
static int parse_single_patch(const char *line, unsigned long size, struct patch *patch)
{
	unsigned long offset = 0;
	unsigned long oldlines = 0, newlines = 0, context = 0;
	struct fragment **fragp = &patch->fragments;

	while (size > 4 && !memcmp(line, "@@ -", 4)) {
		struct fragment *fragment;
		int len;

		fragment = xcalloc(1, sizeof(*fragment));
		fragment->linenr = linenr;
		len = parse_fragment(line, size, patch, fragment);
		if (len <= 0)
			die(_("corrupt patch at line %d"), linenr);
		fragment->patch = line;
		fragment->size = len;
		oldlines += fragment->oldlines;
		newlines += fragment->newlines;
		context += fragment->leading + fragment->trailing;

		*fragp = fragment;
		fragp = &fragment->next;

		offset += len;
		line += len;
		size -= len;
	}

	/*
	 * If something was removed (i.e. we have old-lines) it cannot
	 * be creation, and if something was added it cannot be
	 * deletion.  However, the reverse is not true; --unified=0
	 * patches that only add are not necessarily creation even
	 * though they do not have any old lines, and ones that only
	 * delete are not necessarily deletion.
	 *
	 * Unfortunately, a real creation/deletion patch do _not_ have
	 * any context line by definition, so we cannot safely tell it
	 * apart with --unified=0 insanity.  At least if the patch has
	 * more than one hunk it is not creation or deletion.
	 */
	if (patch->is_new < 0 &&
	    (oldlines || (patch->fragments && patch->fragments->next)))
		patch->is_new = 0;
	if (patch->is_delete < 0 &&
	    (newlines || (patch->fragments && patch->fragments->next)))
		patch->is_delete = 0;

	if (0 < patch->is_new && oldlines)
		die(_("new file %s depends on old contents"), patch->new_name);
	if (0 < patch->is_delete && newlines)
		die(_("deleted file %s still has contents"), patch->old_name);
	if (!patch->is_delete && !newlines && context)
		fprintf_ln(stderr,
			   _("** warning: "
			     "file %s becomes empty but is not deleted"),
			   patch->new_name);

	return offset;
}

static inline int metadata_changes(struct patch *patch)
{
	return	patch->is_rename > 0 ||
		patch->is_copy > 0 ||
		patch->is_new > 0 ||
		patch->is_delete ||
		(patch->old_mode && patch->new_mode &&
		 patch->old_mode != patch->new_mode);
}

static char *inflate_it(const void *data, unsigned long size,
			unsigned long inflated_size)
{
	git_zstream stream;
	void *out;
	int st;

	memset(&stream, 0, sizeof(stream));

	stream.next_in = (unsigned char *)data;
	stream.avail_in = size;
	stream.next_out = out = xmalloc(inflated_size);
	stream.avail_out = inflated_size;
	git_inflate_init(&stream);
	st = git_inflate(&stream, Z_FINISH);
	git_inflate_end(&stream);
	if ((st != Z_STREAM_END) || stream.total_out != inflated_size) {
		free(out);
		return NULL;
	}
	return out;
}

/*
 * Read a binary hunk and return a new fragment; fragment->patch
 * points at an allocated memory that the caller must free, so
 * it is marked as "->free_patch = 1".
 */
static struct fragment *parse_binary_hunk(char **buf_p,
					  unsigned long *sz_p,
					  int *status_p,
					  int *used_p)
{
	/*
	 * Expect a line that begins with binary patch method ("literal"
	 * or "delta"), followed by the length of data before deflating.
	 * a sequence of 'length-byte' followed by base-85 encoded data
	 * should follow, terminated by a newline.
	 *
	 * Each 5-byte sequence of base-85 encodes up to 4 bytes,
	 * and we would limit the patch line to 66 characters,
	 * so one line can fit up to 13 groups that would decode
	 * to 52 bytes max.  The length byte 'A'-'Z' corresponds
	 * to 1-26 bytes, and 'a'-'z' corresponds to 27-52 bytes.
	 */
	int llen, used;
	unsigned long size = *sz_p;
	char *buffer = *buf_p;
	int patch_method;
	unsigned long origlen;
	char *data = NULL;
	int hunk_size = 0;
	struct fragment *frag;

	llen = linelen(buffer, size);
	used = llen;

	*status_p = 0;

	if (starts_with(buffer, "delta ")) {
		patch_method = BINARY_DELTA_DEFLATED;
		origlen = strtoul(buffer + 6, NULL, 10);
	}
	else if (starts_with(buffer, "literal ")) {
		patch_method = BINARY_LITERAL_DEFLATED;
		origlen = strtoul(buffer + 8, NULL, 10);
	}
	else
		return NULL;

	linenr++;
	buffer += llen;
	while (1) {
		int byte_length, max_byte_length, newsize;
		llen = linelen(buffer, size);
		used += llen;
		linenr++;
		if (llen == 1) {
			/* consume the blank line */
			buffer++;
			size--;
			break;
		}
		/*
		 * Minimum line is "A00000\n" which is 7-byte long,
		 * and the line length must be multiple of 5 plus 2.
		 */
		if ((llen < 7) || (llen-2) % 5)
			goto corrupt;
		max_byte_length = (llen - 2) / 5 * 4;
		byte_length = *buffer;
		if ('A' <= byte_length && byte_length <= 'Z')
			byte_length = byte_length - 'A' + 1;
		else if ('a' <= byte_length && byte_length <= 'z')
			byte_length = byte_length - 'a' + 27;
		else
			goto corrupt;
		/* if the input length was not multiple of 4, we would
		 * have filler at the end but the filler should never
		 * exceed 3 bytes
		 */
		if (max_byte_length < byte_length ||
		    byte_length <= max_byte_length - 4)
			goto corrupt;
		newsize = hunk_size + byte_length;
		data = xrealloc(data, newsize);
		if (decode_85(data + hunk_size, buffer + 1, byte_length))
			goto corrupt;
		hunk_size = newsize;
		buffer += llen;
		size -= llen;
	}

	frag = xcalloc(1, sizeof(*frag));
	frag->patch = inflate_it(data, hunk_size, origlen);
	frag->free_patch = 1;
	if (!frag->patch)
		goto corrupt;
	free(data);
	frag->size = origlen;
	*buf_p = buffer;
	*sz_p = size;
	*used_p = used;
	frag->binary_patch_method = patch_method;
	return frag;

 corrupt:
	free(data);
	*status_p = -1;
	error(_("corrupt binary patch at line %d: %.*s"),
	      linenr-1, llen-1, buffer);
	return NULL;
}

static int parse_binary(char *buffer, unsigned long size, struct patch *patch)
{
	/*
	 * We have read "GIT binary patch\n"; what follows is a line
	 * that says the patch method (currently, either "literal" or
	 * "delta") and the length of data before deflating; a
	 * sequence of 'length-byte' followed by base-85 encoded data
	 * follows.
	 *
	 * When a binary patch is reversible, there is another binary
	 * hunk in the same format, starting with patch method (either
	 * "literal" or "delta") with the length of data, and a sequence
	 * of length-byte + base-85 encoded data, terminated with another
	 * empty line.  This data, when applied to the postimage, produces
	 * the preimage.
	 */
	struct fragment *forward;
	struct fragment *reverse;
	int status;
	int used, used_1;

	forward = parse_binary_hunk(&buffer, &size, &status, &used);
	if (!forward && !status)
		/* there has to be one hunk (forward hunk) */
		return error(_("unrecognized binary patch at line %d"), linenr-1);
	if (status)
		/* otherwise we already gave an error message */
		return status;

	reverse = parse_binary_hunk(&buffer, &size, &status, &used_1);
	if (reverse)
		used += used_1;
	else if (status) {
		/*
		 * Not having reverse hunk is not an error, but having
		 * a corrupt reverse hunk is.
		 */
		free((void*) forward->patch);
		free(forward);
		return status;
	}
	forward->next = reverse;
	patch->fragments = forward;
	patch->is_binary = 1;
	return used;
}

static void prefix_one(char **name)
{
	char *old_name = *name;
	if (!old_name)
		return;
	*name = xstrdup(prefix_filename(prefix, prefix_length, *name));
	free(old_name);
}

static void prefix_patch(struct patch *p)
{
	if (!prefix || p->is_toplevel_relative)
		return;
	prefix_one(&p->new_name);
	prefix_one(&p->old_name);
}

/*
 * include/exclude
 */

static struct string_list limit_by_name;
static int has_include;
static void add_name_limit(const char *name, int exclude)
{
	struct string_list_item *it;

	it = string_list_append(&limit_by_name, name);
	it->util = exclude ? NULL : (void *) 1;
}

static int use_patch(struct patch *p)
{
	const char *pathname = p->new_name ? p->new_name : p->old_name;
	int i;

	/* Paths outside are not touched regardless of "--include" */
	if (0 < prefix_length) {
		int pathlen = strlen(pathname);
		if (pathlen <= prefix_length ||
		    memcmp(prefix, pathname, prefix_length))
			return 0;
	}

	/* See if it matches any of exclude/include rule */
	for (i = 0; i < limit_by_name.nr; i++) {
		struct string_list_item *it = &limit_by_name.items[i];
		if (!wildmatch(it->string, pathname, 0, NULL))
			return (it->util != NULL);
	}

	/*
	 * If we had any include, a path that does not match any rule is
	 * not used.  Otherwise, we saw bunch of exclude rules (or none)
	 * and such a path is used.
	 */
	return !has_include;
}


/*
 * Read the patch text in "buffer" that extends for "size" bytes; stop
 * reading after seeing a single patch (i.e. changes to a single file).
 * Create fragments (i.e. patch hunks) and hang them to the given patch.
 * Return the number of bytes consumed, so that the caller can call us
 * again for the next patch.
 */
static int parse_chunk(char *buffer, unsigned long size, struct patch *patch)
{
	int hdrsize, patchsize;
	int offset = find_header(buffer, size, &hdrsize, patch);

	if (offset < 0)
		return offset;

	prefix_patch(patch);

	if (!use_patch(patch))
		patch->ws_rule = 0;
	else
		patch->ws_rule = whitespace_rule(patch->new_name
						 ? patch->new_name
						 : patch->old_name);

	patchsize = parse_single_patch(buffer + offset + hdrsize,
				       size - offset - hdrsize, patch);

	if (!patchsize) {
		static const char git_binary[] = "GIT binary patch\n";
		int hd = hdrsize + offset;
		unsigned long llen = linelen(buffer + hd, size - hd);

		if (llen == sizeof(git_binary) - 1 &&
		    !memcmp(git_binary, buffer + hd, llen)) {
			int used;
			linenr++;
			used = parse_binary(buffer + hd + llen,
					    size - hd - llen, patch);
			if (used)
				patchsize = used + llen;
			else
				patchsize = 0;
		}
		else if (!memcmp(" differ\n", buffer + hd + llen - 8, 8)) {
			static const char *binhdr[] = {
				"Binary files ",
				"Files ",
				NULL,
			};
			int i;
			for (i = 0; binhdr[i]; i++) {
				int len = strlen(binhdr[i]);
				if (len < size - hd &&
				    !memcmp(binhdr[i], buffer + hd, len)) {
					linenr++;
					patch->is_binary = 1;
					patchsize = llen;
					break;
				}
			}
		}

		/* Empty patch cannot be applied if it is a text patch
		 * without metadata change.  A binary patch appears
		 * empty to us here.
		 */
		if ((apply || check) &&
		    (!patch->is_binary && !metadata_changes(patch)))
			die(_("patch with only garbage at line %d"), linenr);
	}

	return offset + hdrsize + patchsize;
}

#define swap(a,b) myswap((a),(b),sizeof(a))

#define myswap(a, b, size) do {		\
	unsigned char mytmp[size];	\
	memcpy(mytmp, &a, size);		\
	memcpy(&a, &b, size);		\
	memcpy(&b, mytmp, size);		\
} while (0)

static void reverse_patches(struct patch *p)
{
	for (; p; p = p->next) {
		struct fragment *frag = p->fragments;

		swap(p->new_name, p->old_name);
		swap(p->new_mode, p->old_mode);
		swap(p->is_new, p->is_delete);
		swap(p->lines_added, p->lines_deleted);
		swap(p->old_sha1_prefix, p->new_sha1_prefix);

		for (; frag; frag = frag->next) {
			swap(frag->newpos, frag->oldpos);
			swap(frag->newlines, frag->oldlines);
		}
	}
}

static const char pluses[] =
"++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++";
static const char minuses[]=
"----------------------------------------------------------------------";

static void show_stats(struct patch *patch)
{
	struct strbuf qname = STRBUF_INIT;
	char *cp = patch->new_name ? patch->new_name : patch->old_name;
	int max, add, del;

	quote_c_style(cp, &qname, NULL, 0);

	/*
	 * "scale" the filename
	 */
	max = max_len;
	if (max > 50)
		max = 50;

	if (qname.len > max) {
		cp = strchr(qname.buf + qname.len + 3 - max, '/');
		if (!cp)
			cp = qname.buf + qname.len + 3 - max;
		strbuf_splice(&qname, 0, cp - qname.buf, "...", 3);
	}

	if (patch->is_binary) {
		printf(" %-*s |  Bin\n", max, qname.buf);
		strbuf_release(&qname);
		return;
	}

	printf(" %-*s |", max, qname.buf);
	strbuf_release(&qname);

	/*
	 * scale the add/delete
	 */
	max = max + max_change > 70 ? 70 - max : max_change;
	add = patch->lines_added;
	del = patch->lines_deleted;

	if (max_change > 0) {
		int total = ((add + del) * max + max_change / 2) / max_change;
		add = (add * max + max_change / 2) / max_change;
		del = total - add;
	}
	printf("%5d %.*s%.*s\n", patch->lines_added + patch->lines_deleted,
		add, pluses, del, minuses);
}

static int read_old_data(struct stat *st, const char *path, struct strbuf *buf)
{
	switch (st->st_mode & S_IFMT) {
	case S_IFLNK:
		if (strbuf_readlink(buf, path, st->st_size) < 0)
			return error(_("unable to read symlink %s"), path);
		return 0;
	case S_IFREG:
		if (strbuf_read_file(buf, path, st->st_size) != st->st_size)
			return error(_("unable to open or read %s"), path);
		convert_to_git(path, buf->buf, buf->len, buf, 0);
		return 0;
	default:
		return -1;
	}
}

/*
 * Update the preimage, and the common lines in postimage,
 * from buffer buf of length len. If postlen is 0 the postimage
 * is updated in place, otherwise it's updated on a new buffer
 * of length postlen
 */

static void update_pre_post_images(struct image *preimage,
				   struct image *postimage,
				   char *buf,
				   size_t len, size_t postlen)
{
	int i, ctx, reduced;
	char *new, *old, *fixed;
	struct image fixed_preimage;

	/*
	 * Update the preimage with whitespace fixes.  Note that we
	 * are not losing preimage->buf -- apply_one_fragment() will
	 * free "oldlines".
	 */
	prepare_image(&fixed_preimage, buf, len, 1);
	assert(postlen
	       ? fixed_preimage.nr == preimage->nr
	       : fixed_preimage.nr <= preimage->nr);
	for (i = 0; i < fixed_preimage.nr; i++)
		fixed_preimage.line[i].flag = preimage->line[i].flag;
	free(preimage->line_allocated);
	*preimage = fixed_preimage;

	/*
	 * Adjust the common context lines in postimage. This can be
	 * done in-place when we are shrinking it with whitespace
	 * fixing, but needs a new buffer when ignoring whitespace or
	 * expanding leading tabs to spaces.
	 *
	 * We trust the caller to tell us if the update can be done
	 * in place (postlen==0) or not.
	 */
	old = postimage->buf;
	if (postlen)
		new = postimage->buf = xmalloc(postlen);
	else
		new = old;
	fixed = preimage->buf;

	for (i = reduced = ctx = 0; i < postimage->nr; i++) {
		size_t len = postimage->line[i].len;
		if (!(postimage->line[i].flag & LINE_COMMON)) {
			/* an added line -- no counterparts in preimage */
			memmove(new, old, len);
			old += len;
			new += len;
			continue;
		}

		/* a common context -- skip it in the original postimage */
		old += len;

		/* and find the corresponding one in the fixed preimage */
		while (ctx < preimage->nr &&
		       !(preimage->line[ctx].flag & LINE_COMMON)) {
			fixed += preimage->line[ctx].len;
			ctx++;
		}

		/*
		 * preimage is expected to run out, if the caller
		 * fixed addition of trailing blank lines.
		 */
		if (preimage->nr <= ctx) {
			reduced++;
			continue;
		}

		/* and copy it in, while fixing the line length */
		len = preimage->line[ctx].len;
		memcpy(new, fixed, len);
		new += len;
		fixed += len;
		postimage->line[i].len = len;
		ctx++;
	}

	if (postlen
	    ? postlen < new - postimage->buf
	    : postimage->len < new - postimage->buf)
		die("BUG: caller miscounted postlen: asked %d, orig = %d, used = %d",
		    (int)postlen, (int) postimage->len, (int)(new - postimage->buf));

	/* Fix the length of the whole thing */
	postimage->len = new - postimage->buf;
	postimage->nr -= reduced;
}

static int match_fragment(struct image *img,
			  struct image *preimage,
			  struct image *postimage,
			  unsigned long try,
			  int try_lno,
			  unsigned ws_rule,
			  int match_beginning, int match_end)
{
	int i;
	char *fixed_buf, *buf, *orig, *target;
	struct strbuf fixed;
	size_t fixed_len, postlen;
	int preimage_limit;

	if (preimage->nr + try_lno <= img->nr) {
		/*
		 * The hunk falls within the boundaries of img.
		 */
		preimage_limit = preimage->nr;
		if (match_end && (preimage->nr + try_lno != img->nr))
			return 0;
	} else if (ws_error_action == correct_ws_error &&
		   (ws_rule & WS_BLANK_AT_EOF)) {
		/*
		 * This hunk extends beyond the end of img, and we are
		 * removing blank lines at the end of the file.  This
		 * many lines from the beginning of the preimage must
		 * match with img, and the remainder of the preimage
		 * must be blank.
		 */
		preimage_limit = img->nr - try_lno;
	} else {
		/*
		 * The hunk extends beyond the end of the img and
		 * we are not removing blanks at the end, so we
		 * should reject the hunk at this position.
		 */
		return 0;
	}

	if (match_beginning && try_lno)
		return 0;

	/* Quick hash check */
	for (i = 0; i < preimage_limit; i++)
		if ((img->line[try_lno + i].flag & LINE_PATCHED) ||
		    (preimage->line[i].hash != img->line[try_lno + i].hash))
			return 0;

	if (preimage_limit == preimage->nr) {
		/*
		 * Do we have an exact match?  If we were told to match
		 * at the end, size must be exactly at try+fragsize,
		 * otherwise try+fragsize must be still within the preimage,
		 * and either case, the old piece should match the preimage
		 * exactly.
		 */
		if ((match_end
		     ? (try + preimage->len == img->len)
		     : (try + preimage->len <= img->len)) &&
		    !memcmp(img->buf + try, preimage->buf, preimage->len))
			return 1;
	} else {
		/*
		 * The preimage extends beyond the end of img, so
		 * there cannot be an exact match.
		 *
		 * There must be one non-blank context line that match
		 * a line before the end of img.
		 */
		char *buf_end;

		buf = preimage->buf;
		buf_end = buf;
		for (i = 0; i < preimage_limit; i++)
			buf_end += preimage->line[i].len;

		for ( ; buf < buf_end; buf++)
			if (!isspace(*buf))
				break;
		if (buf == buf_end)
			return 0;
	}

	/*
	 * No exact match. If we are ignoring whitespace, run a line-by-line
	 * fuzzy matching. We collect all the line length information because
	 * we need it to adjust whitespace if we match.
	 */
	if (ws_ignore_action == ignore_ws_change) {
		size_t imgoff = 0;
		size_t preoff = 0;
		size_t postlen = postimage->len;
		size_t extra_chars;
		char *preimage_eof;
		char *preimage_end;
		for (i = 0; i < preimage_limit; i++) {
			size_t prelen = preimage->line[i].len;
			size_t imglen = img->line[try_lno+i].len;

			if (!fuzzy_matchlines(img->buf + try + imgoff, imglen,
					      preimage->buf + preoff, prelen))
				return 0;
			if (preimage->line[i].flag & LINE_COMMON)
				postlen += imglen - prelen;
			imgoff += imglen;
			preoff += prelen;
		}

		/*
		 * Ok, the preimage matches with whitespace fuzz.
		 *
		 * imgoff now holds the true length of the target that
		 * matches the preimage before the end of the file.
		 *
		 * Count the number of characters in the preimage that fall
		 * beyond the end of the file and make sure that all of them
		 * are whitespace characters. (This can only happen if
		 * we are removing blank lines at the end of the file.)
		 */
		buf = preimage_eof = preimage->buf + preoff;
		for ( ; i < preimage->nr; i++)
			preoff += preimage->line[i].len;
		preimage_end = preimage->buf + preoff;
		for ( ; buf < preimage_end; buf++)
			if (!isspace(*buf))
				return 0;

		/*
		 * Update the preimage and the common postimage context
		 * lines to use the same whitespace as the target.
		 * If whitespace is missing in the target (i.e.
		 * if the preimage extends beyond the end of the file),
		 * use the whitespace from the preimage.
		 */
		extra_chars = preimage_end - preimage_eof;
		strbuf_init(&fixed, imgoff + extra_chars);
		strbuf_add(&fixed, img->buf + try, imgoff);
		strbuf_add(&fixed, preimage_eof, extra_chars);
		fixed_buf = strbuf_detach(&fixed, &fixed_len);
		update_pre_post_images(preimage, postimage,
				fixed_buf, fixed_len, postlen);
		return 1;
	}

	if (ws_error_action != correct_ws_error)
		return 0;

	/*
	 * The hunk does not apply byte-by-byte, but the hash says
	 * it might with whitespace fuzz. We weren't asked to
	 * ignore whitespace, we were asked to correct whitespace
	 * errors, so let's try matching after whitespace correction.
	 *
	 * While checking the preimage against the target, whitespace
	 * errors in both fixed, we count how large the corresponding
	 * postimage needs to be.  The postimage prepared by
	 * apply_one_fragment() has whitespace errors fixed on added
	 * lines already, but the common lines were propagated as-is,
	 * which may become longer when their whitespace errors are
	 * fixed.
	 */

	/* First count added lines in postimage */
	postlen = 0;
	for (i = 0; i < postimage->nr; i++) {
		if (!(postimage->line[i].flag & LINE_COMMON))
			postlen += postimage->line[i].len;
	}

	/*
	 * The preimage may extend beyond the end of the file,
	 * but in this loop we will only handle the part of the
	 * preimage that falls within the file.
	 */
	strbuf_init(&fixed, preimage->len + 1);
	orig = preimage->buf;
	target = img->buf + try;
	for (i = 0; i < preimage_limit; i++) {
		size_t oldlen = preimage->line[i].len;
		size_t tgtlen = img->line[try_lno + i].len;
		size_t fixstart = fixed.len;
		struct strbuf tgtfix;
		int match;

		/* Try fixing the line in the preimage */
		ws_fix_copy(&fixed, orig, oldlen, ws_rule, NULL);

		/* Try fixing the line in the target */
		strbuf_init(&tgtfix, tgtlen);
		ws_fix_copy(&tgtfix, target, tgtlen, ws_rule, NULL);

		/*
		 * If they match, either the preimage was based on
		 * a version before our tree fixed whitespace breakage,
		 * or we are lacking a whitespace-fix patch the tree
		 * the preimage was based on already had (i.e. target
		 * has whitespace breakage, the preimage doesn't).
		 * In either case, we are fixing the whitespace breakages
		 * so we might as well take the fix together with their
		 * real change.
		 */
		match = (tgtfix.len == fixed.len - fixstart &&
			 !memcmp(tgtfix.buf, fixed.buf + fixstart,
					     fixed.len - fixstart));

		/* Add the length if this is common with the postimage */
		if (preimage->line[i].flag & LINE_COMMON)
			postlen += tgtfix.len;

		strbuf_release(&tgtfix);
		if (!match)
			goto unmatch_exit;

		orig += oldlen;
		target += tgtlen;
	}


	/*
	 * Now handle the lines in the preimage that falls beyond the
	 * end of the file (if any). They will only match if they are
	 * empty or only contain whitespace (if WS_BLANK_AT_EOL is
	 * false).
	 */
	for ( ; i < preimage->nr; i++) {
		size_t fixstart = fixed.len; /* start of the fixed preimage */
		size_t oldlen = preimage->line[i].len;
		int j;

		/* Try fixing the line in the preimage */
		ws_fix_copy(&fixed, orig, oldlen, ws_rule, NULL);

		for (j = fixstart; j < fixed.len; j++)
			if (!isspace(fixed.buf[j]))
				goto unmatch_exit;

		orig += oldlen;
	}

	/*
	 * Yes, the preimage is based on an older version that still
	 * has whitespace breakages unfixed, and fixing them makes the
	 * hunk match.  Update the context lines in the postimage.
	 */
	fixed_buf = strbuf_detach(&fixed, &fixed_len);
	if (postlen < postimage->len)
		postlen = 0;
	update_pre_post_images(preimage, postimage,
			       fixed_buf, fixed_len, postlen);
	return 1;

 unmatch_exit:
	strbuf_release(&fixed);
	return 0;
}

static int find_pos(struct image *img,
		    struct image *preimage,
		    struct image *postimage,
		    int line,
		    unsigned ws_rule,
		    int match_beginning, int match_end)
{
	int i;
	unsigned long backwards, forwards, try;
	int backwards_lno, forwards_lno, try_lno;

	/*
	 * If match_beginning or match_end is specified, there is no
	 * point starting from a wrong line that will never match and
	 * wander around and wait for a match at the specified end.
	 */
	if (match_beginning)
		line = 0;
	else if (match_end)
		line = img->nr - preimage->nr;

	/*
	 * Because the comparison is unsigned, the following test
	 * will also take care of a negative line number that can
	 * result when match_end and preimage is larger than the target.
	 */
	if ((size_t) line > img->nr)
		line = img->nr;

	try = 0;
	for (i = 0; i < line; i++)
		try += img->line[i].len;

	/*
	 * There's probably some smart way to do this, but I'll leave
	 * that to the smart and beautiful people. I'm simple and stupid.
	 */
	backwards = try;
	backwards_lno = line;
	forwards = try;
	forwards_lno = line;
	try_lno = line;

	for (i = 0; ; i++) {
		if (match_fragment(img, preimage, postimage,
				   try, try_lno, ws_rule,
				   match_beginning, match_end))
			return try_lno;

	again:
		if (backwards_lno == 0 && forwards_lno == img->nr)
			break;

		if (i & 1) {
			if (backwards_lno == 0) {
				i++;
				goto again;
			}
			backwards_lno--;
			backwards -= img->line[backwards_lno].len;
			try = backwards;
			try_lno = backwards_lno;
		} else {
			if (forwards_lno == img->nr) {
				i++;
				goto again;
			}
			forwards += img->line[forwards_lno].len;
			forwards_lno++;
			try = forwards;
			try_lno = forwards_lno;
		}

	}
	return -1;
}

static void remove_first_line(struct image *img)
{
	img->buf += img->line[0].len;
	img->len -= img->line[0].len;
	img->line++;
	img->nr--;
}

static void remove_last_line(struct image *img)
{
	img->len -= img->line[--img->nr].len;
}

/*
 * The change from "preimage" and "postimage" has been found to
 * apply at applied_pos (counts in line numbers) in "img".
 * Update "img" to remove "preimage" and replace it with "postimage".
 */
static void update_image(struct image *img,
			 int applied_pos,
			 struct image *preimage,
			 struct image *postimage)
{
	/*
	 * remove the copy of preimage at offset in img
	 * and replace it with postimage
	 */
	int i, nr;
	size_t remove_count, insert_count, applied_at = 0;
	char *result;
	int preimage_limit;

	/*
	 * If we are removing blank lines at the end of img,
	 * the preimage may extend beyond the end.
	 * If that is the case, we must be careful only to
	 * remove the part of the preimage that falls within
	 * the boundaries of img. Initialize preimage_limit
	 * to the number of lines in the preimage that falls
	 * within the boundaries.
	 */
	preimage_limit = preimage->nr;
	if (preimage_limit > img->nr - applied_pos)
		preimage_limit = img->nr - applied_pos;

	for (i = 0; i < applied_pos; i++)
		applied_at += img->line[i].len;

	remove_count = 0;
	for (i = 0; i < preimage_limit; i++)
		remove_count += img->line[applied_pos + i].len;
	insert_count = postimage->len;

	/* Adjust the contents */
	result = xmalloc(img->len + insert_count - remove_count + 1);
	memcpy(result, img->buf, applied_at);
	memcpy(result + applied_at, postimage->buf, postimage->len);
	memcpy(result + applied_at + postimage->len,
	       img->buf + (applied_at + remove_count),
	       img->len - (applied_at + remove_count));
	free(img->buf);
	img->buf = result;
	img->len += insert_count - remove_count;
	result[img->len] = '\0';

	/* Adjust the line table */
	nr = img->nr + postimage->nr - preimage_limit;
	if (preimage_limit < postimage->nr) {
		/*
		 * NOTE: this knows that we never call remove_first_line()
		 * on anything other than pre/post image.
		 */
		REALLOC_ARRAY(img->line, nr);
		img->line_allocated = img->line;
	}
	if (preimage_limit != postimage->nr)
		memmove(img->line + applied_pos + postimage->nr,
			img->line + applied_pos + preimage_limit,
			(img->nr - (applied_pos + preimage_limit)) *
			sizeof(*img->line));
	memcpy(img->line + applied_pos,
	       postimage->line,
	       postimage->nr * sizeof(*img->line));
	if (!allow_overlap)
		for (i = 0; i < postimage->nr; i++)
			img->line[applied_pos + i].flag |= LINE_PATCHED;
	img->nr = nr;
}

/*
 * Use the patch-hunk text in "frag" to prepare two images (preimage and
 * postimage) for the hunk.  Find lines that match "preimage" in "img" and
 * replace the part of "img" with "postimage" text.
 */
static int apply_one_fragment(struct image *img, struct fragment *frag,
			      int inaccurate_eof, unsigned ws_rule,
			      int nth_fragment)
{
	int match_beginning, match_end;
	const char *patch = frag->patch;
	int size = frag->size;
	char *old, *oldlines;
	struct strbuf newlines;
	int new_blank_lines_at_end = 0;
	int found_new_blank_lines_at_end = 0;
	int hunk_linenr = frag->linenr;
	unsigned long leading, trailing;
	int pos, applied_pos;
	struct image preimage;
	struct image postimage;

	memset(&preimage, 0, sizeof(preimage));
	memset(&postimage, 0, sizeof(postimage));
	oldlines = xmalloc(size);
	strbuf_init(&newlines, size);

	old = oldlines;
	while (size > 0) {
		char first;
		int len = linelen(patch, size);
		int plen;
		int added_blank_line = 0;
		int is_blank_context = 0;
		size_t start;

		if (!len)
			break;

		/*
		 * "plen" is how much of the line we should use for
		 * the actual patch data. Normally we just remove the
		 * first character on the line, but if the line is
		 * followed by "\ No newline", then we also remove the
		 * last one (which is the newline, of course).
		 */
		plen = len - 1;
		if (len < size && patch[len] == '\\')
			plen--;
		first = *patch;
		if (apply_in_reverse) {
			if (first == '-')
				first = '+';
			else if (first == '+')
				first = '-';
		}

		switch (first) {
		case '\n':
			/* Newer GNU diff, empty context line */
			if (plen < 0)
				/* ... followed by '\No newline'; nothing */
				break;
			*old++ = '\n';
			strbuf_addch(&newlines, '\n');
			add_line_info(&preimage, "\n", 1, LINE_COMMON);
			add_line_info(&postimage, "\n", 1, LINE_COMMON);
			is_blank_context = 1;
			break;
		case ' ':
			if (plen && (ws_rule & WS_BLANK_AT_EOF) &&
			    ws_blank_line(patch + 1, plen, ws_rule))
				is_blank_context = 1;
		case '-':
			memcpy(old, patch + 1, plen);
			add_line_info(&preimage, old, plen,
				      (first == ' ' ? LINE_COMMON : 0));
			old += plen;
			if (first == '-')
				break;
		/* Fall-through for ' ' */
		case '+':
			/* --no-add does not add new lines */
			if (first == '+' && no_add)
				break;

			start = newlines.len;
			if (first != '+' ||
			    !whitespace_error ||
			    ws_error_action != correct_ws_error) {
				strbuf_add(&newlines, patch + 1, plen);
			}
			else {
				ws_fix_copy(&newlines, patch + 1, plen, ws_rule, &applied_after_fixing_ws);
			}
			add_line_info(&postimage, newlines.buf + start, newlines.len - start,
				      (first == '+' ? 0 : LINE_COMMON));
			if (first == '+' &&
			    (ws_rule & WS_BLANK_AT_EOF) &&
			    ws_blank_line(patch + 1, plen, ws_rule))
				added_blank_line = 1;
			break;
		case '@': case '\\':
			/* Ignore it, we already handled it */
			break;
		default:
			if (apply_verbosely)
				error(_("invalid start of line: '%c'"), first);
			applied_pos = -1;
			goto out;
		}
		if (added_blank_line) {
			if (!new_blank_lines_at_end)
				found_new_blank_lines_at_end = hunk_linenr;
			new_blank_lines_at_end++;
		}
		else if (is_blank_context)
			;
		else
			new_blank_lines_at_end = 0;
		patch += len;
		size -= len;
		hunk_linenr++;
	}
	if (inaccurate_eof &&
	    old > oldlines && old[-1] == '\n' &&
	    newlines.len > 0 && newlines.buf[newlines.len - 1] == '\n') {
		old--;
		strbuf_setlen(&newlines, newlines.len - 1);
	}

	leading = frag->leading;
	trailing = frag->trailing;

	/*
	 * A hunk to change lines at the beginning would begin with
	 * @@ -1,L +N,M @@
	 * but we need to be careful.  -U0 that inserts before the second
	 * line also has this pattern.
	 *
	 * And a hunk to add to an empty file would begin with
	 * @@ -0,0 +N,M @@
	 *
	 * In other words, a hunk that is (frag->oldpos <= 1) with or
	 * without leading context must match at the beginning.
	 */
	match_beginning = (!frag->oldpos ||
			   (frag->oldpos == 1 && !unidiff_zero));

	/*
	 * A hunk without trailing lines must match at the end.
	 * However, we simply cannot tell if a hunk must match end
	 * from the lack of trailing lines if the patch was generated
	 * with unidiff without any context.
	 */
	match_end = !unidiff_zero && !trailing;

	pos = frag->newpos ? (frag->newpos - 1) : 0;
	preimage.buf = oldlines;
	preimage.len = old - oldlines;
	postimage.buf = newlines.buf;
	postimage.len = newlines.len;
	preimage.line = preimage.line_allocated;
	postimage.line = postimage.line_allocated;

	for (;;) {

		applied_pos = find_pos(img, &preimage, &postimage, pos,
				       ws_rule, match_beginning, match_end);

		if (applied_pos >= 0)
			break;

		/* Am I at my context limits? */
		if ((leading <= p_context) && (trailing <= p_context))
			break;
		if (match_beginning || match_end) {
			match_beginning = match_end = 0;
			continue;
		}

		/*
		 * Reduce the number of context lines; reduce both
		 * leading and trailing if they are equal otherwise
		 * just reduce the larger context.
		 */
		if (leading >= trailing) {
			remove_first_line(&preimage);
			remove_first_line(&postimage);
			pos--;
			leading--;
		}
		if (trailing > leading) {
			remove_last_line(&preimage);
			remove_last_line(&postimage);
			trailing--;
		}
	}

	if (applied_pos >= 0) {
		if (new_blank_lines_at_end &&
		    preimage.nr + applied_pos >= img->nr &&
		    (ws_rule & WS_BLANK_AT_EOF) &&
		    ws_error_action != nowarn_ws_error) {
			record_ws_error(WS_BLANK_AT_EOF, "+", 1,
					found_new_blank_lines_at_end);
			if (ws_error_action == correct_ws_error) {
				while (new_blank_lines_at_end--)
					remove_last_line(&postimage);
			}
			/*
			 * We would want to prevent write_out_results()
			 * from taking place in apply_patch() that follows
			 * the callchain led us here, which is:
			 * apply_patch->check_patch_list->check_patch->
			 * apply_data->apply_fragments->apply_one_fragment
			 */
			if (ws_error_action == die_on_ws_error)
				apply = 0;
		}

		if (apply_verbosely && applied_pos != pos) {
			int offset = applied_pos - pos;
			if (apply_in_reverse)
				offset = 0 - offset;
			fprintf_ln(stderr,
				   Q_("Hunk #%d succeeded at %d (offset %d line).",
				      "Hunk #%d succeeded at %d (offset %d lines).",
				      offset),
				   nth_fragment, applied_pos + 1, offset);
		}

		/*
		 * Warn if it was necessary to reduce the number
		 * of context lines.
		 */
		if ((leading != frag->leading) ||
		    (trailing != frag->trailing))
			fprintf_ln(stderr, _("Context reduced to (%ld/%ld)"
					     " to apply fragment at %d"),
				   leading, trailing, applied_pos+1);
		update_image(img, applied_pos, &preimage, &postimage);
	} else {
		if (apply_verbosely)
			error(_("while searching for:\n%.*s"),
			      (int)(old - oldlines), oldlines);
	}

out:
	free(oldlines);
	strbuf_release(&newlines);
	free(preimage.line_allocated);
	free(postimage.line_allocated);

	return (applied_pos < 0);
}

static int apply_binary_fragment(struct image *img, struct patch *patch)
{
	struct fragment *fragment = patch->fragments;
	unsigned long len;
	void *dst;

	if (!fragment)
		return error(_("missing binary patch data for '%s'"),
			     patch->new_name ?
			     patch->new_name :
			     patch->old_name);

	/* Binary patch is irreversible without the optional second hunk */
	if (apply_in_reverse) {
		if (!fragment->next)
			return error("cannot reverse-apply a binary patch "
				     "without the reverse hunk to '%s'",
				     patch->new_name
				     ? patch->new_name : patch->old_name);
		fragment = fragment->next;
	}
	switch (fragment->binary_patch_method) {
	case BINARY_DELTA_DEFLATED:
		dst = patch_delta(img->buf, img->len, fragment->patch,
				  fragment->size, &len);
		if (!dst)
			return -1;
		clear_image(img);
		img->buf = dst;
		img->len = len;
		return 0;
	case BINARY_LITERAL_DEFLATED:
		clear_image(img);
		img->len = fragment->size;
		img->buf = xmemdupz(fragment->patch, img->len);
		return 0;
	}
	return -1;
}

/*
 * Replace "img" with the result of applying the binary patch.
 * The binary patch data itself in patch->fragment is still kept
 * but the preimage prepared by the caller in "img" is freed here
 * or in the helper function apply_binary_fragment() this calls.
 */
static int apply_binary(struct image *img, struct patch *patch)
{
	const char *name = patch->old_name ? patch->old_name : patch->new_name;
	unsigned char sha1[20];

	/*
	 * For safety, we require patch index line to contain
	 * full 40-byte textual SHA1 for old and new, at least for now.
	 */
	if (strlen(patch->old_sha1_prefix) != 40 ||
	    strlen(patch->new_sha1_prefix) != 40 ||
	    get_sha1_hex(patch->old_sha1_prefix, sha1) ||
	    get_sha1_hex(patch->new_sha1_prefix, sha1))
		return error("cannot apply binary patch to '%s' "
			     "without full index line", name);

	if (patch->old_name) {
		/*
		 * See if the old one matches what the patch
		 * applies to.
		 */
		hash_sha1_file(img->buf, img->len, blob_type, sha1);
		if (strcmp(sha1_to_hex(sha1), patch->old_sha1_prefix))
			return error("the patch applies to '%s' (%s), "
				     "which does not match the "
				     "current contents.",
				     name, sha1_to_hex(sha1));
	}
	else {
		/* Otherwise, the old one must be empty. */
		if (img->len)
			return error("the patch applies to an empty "
				     "'%s' but it is not empty", name);
	}

	get_sha1_hex(patch->new_sha1_prefix, sha1);
	if (is_null_sha1(sha1)) {
		clear_image(img);
		return 0; /* deletion patch */
	}

	if (has_sha1_file(sha1)) {
		/* We already have the postimage */
		enum object_type type;
		unsigned long size;
		char *result;

		result = read_sha1_file(sha1, &type, &size);
		if (!result)
			return error("the necessary postimage %s for "
				     "'%s' cannot be read",
				     patch->new_sha1_prefix, name);
		clear_image(img);
		img->buf = result;
		img->len = size;
	} else {
		/*
		 * We have verified buf matches the preimage;
		 * apply the patch data to it, which is stored
		 * in the patch->fragments->{patch,size}.
		 */
		if (apply_binary_fragment(img, patch))
			return error(_("binary patch does not apply to '%s'"),
				     name);

		/* verify that the result matches */
		hash_sha1_file(img->buf, img->len, blob_type, sha1);
		if (strcmp(sha1_to_hex(sha1), patch->new_sha1_prefix))
			return error(_("binary patch to '%s' creates incorrect result (expecting %s, got %s)"),
				name, patch->new_sha1_prefix, sha1_to_hex(sha1));
	}

	return 0;
}

static int apply_fragments(struct image *img, struct patch *patch)
{
	struct fragment *frag = patch->fragments;
	const char *name = patch->old_name ? patch->old_name : patch->new_name;
	unsigned ws_rule = patch->ws_rule;
	unsigned inaccurate_eof = patch->inaccurate_eof;
	int nth = 0;

	if (patch->is_binary)
		return apply_binary(img, patch);

	while (frag) {
		nth++;
		if (apply_one_fragment(img, frag, inaccurate_eof, ws_rule, nth)) {
			error(_("patch failed: %s:%ld"), name, frag->oldpos);
			if (!apply_with_reject)
				return -1;
			frag->rejected = 1;
		}
		frag = frag->next;
	}
	return 0;
}

static int read_blob_object(struct strbuf *buf, const unsigned char *sha1, unsigned mode)
{
	if (S_ISGITLINK(mode)) {
		strbuf_grow(buf, 100);
		strbuf_addf(buf, "Subproject commit %s\n", sha1_to_hex(sha1));
	} else {
		enum object_type type;
		unsigned long sz;
		char *result;

		result = read_sha1_file(sha1, &type, &sz);
		if (!result)
			return -1;
		/* XXX read_sha1_file NUL-terminates */
		strbuf_attach(buf, result, sz, sz + 1);
	}
	return 0;
}

static int read_file_or_gitlink(const struct cache_entry *ce, struct strbuf *buf)
{
	if (!ce)
		return 0;
	return read_blob_object(buf, ce->sha1, ce->ce_mode);
}

static struct patch *in_fn_table(const char *name)
{
	struct string_list_item *item;

	if (name == NULL)
		return NULL;

	item = string_list_lookup(&fn_table, name);
	if (item != NULL)
		return (struct patch *)item->util;

	return NULL;
}

/*
 * item->util in the filename table records the status of the path.
 * Usually it points at a patch (whose result records the contents
 * of it after applying it), but it could be PATH_WAS_DELETED for a
 * path that a previously applied patch has already removed, or
 * PATH_TO_BE_DELETED for a path that a later patch would remove.
 *
 * The latter is needed to deal with a case where two paths A and B
 * are swapped by first renaming A to B and then renaming B to A;
 * moving A to B should not be prevented due to presence of B as we
 * will remove it in a later patch.
 */
#define PATH_TO_BE_DELETED ((struct patch *) -2)
#define PATH_WAS_DELETED ((struct patch *) -1)

static int to_be_deleted(struct patch *patch)
{
	return patch == PATH_TO_BE_DELETED;
}

static int was_deleted(struct patch *patch)
{
	return patch == PATH_WAS_DELETED;
}

static void add_to_fn_table(struct patch *patch)
{
	struct string_list_item *item;

	/*
	 * Always add new_name unless patch is a deletion
	 * This should cover the cases for normal diffs,
	 * file creations and copies
	 */
	if (patch->new_name != NULL) {
		item = string_list_insert(&fn_table, patch->new_name);
		item->util = patch;
	}

	/*
	 * store a failure on rename/deletion cases because
	 * later chunks shouldn't patch old names
	 */
	if ((patch->new_name == NULL) || (patch->is_rename)) {
		item = string_list_insert(&fn_table, patch->old_name);
		item->util = PATH_WAS_DELETED;
	}
}

static void prepare_fn_table(struct patch *patch)
{
	/*
	 * store information about incoming file deletion
	 */
	while (patch) {
		if ((patch->new_name == NULL) || (patch->is_rename)) {
			struct string_list_item *item;
			item = string_list_insert(&fn_table, patch->old_name);
			item->util = PATH_TO_BE_DELETED;
		}
		patch = patch->next;
	}
}

static int checkout_target(struct index_state *istate,
			   struct cache_entry *ce, struct stat *st)
{
	struct checkout costate;

	memset(&costate, 0, sizeof(costate));
	costate.base_dir = "";
	costate.refresh_cache = 1;
	costate.istate = istate;
	if (checkout_entry(ce, &costate, NULL) || lstat(ce->name, st))
		return error(_("cannot checkout %s"), ce->name);
	return 0;
}

static struct patch *previous_patch(struct patch *patch, int *gone)
{
	struct patch *previous;

	*gone = 0;
	if (patch->is_copy || patch->is_rename)
		return NULL; /* "git" patches do not depend on the order */

	previous = in_fn_table(patch->old_name);
	if (!previous)
		return NULL;

	if (to_be_deleted(previous))
		return NULL; /* the deletion hasn't happened yet */

	if (was_deleted(previous))
		*gone = 1;

	return previous;
}

static int verify_index_match(const struct cache_entry *ce, struct stat *st)
{
	if (S_ISGITLINK(ce->ce_mode)) {
		if (!S_ISDIR(st->st_mode))
			return -1;
		return 0;
	}
	return ce_match_stat(ce, st, CE_MATCH_IGNORE_VALID|CE_MATCH_IGNORE_SKIP_WORKTREE);
}

#define SUBMODULE_PATCH_WITHOUT_INDEX 1

static int load_patch_target(struct strbuf *buf,
			     const struct cache_entry *ce,
			     struct stat *st,
			     const char *name,
			     unsigned expected_mode)
{
	if (cached || check_index) {
		if (read_file_or_gitlink(ce, buf))
			return error(_("read of %s failed"), name);
	} else if (name) {
		if (S_ISGITLINK(expected_mode)) {
			if (ce)
				return read_file_or_gitlink(ce, buf);
			else
				return SUBMODULE_PATCH_WITHOUT_INDEX;
		} else if (has_symlink_leading_path(name, strlen(name))) {
			return error(_("reading from '%s' beyond a symbolic link"), name);
		} else {
			if (read_old_data(st, name, buf))
				return error(_("read of %s failed"), name);
		}
	}
	return 0;
}

/*
 * We are about to apply "patch"; populate the "image" with the
 * current version we have, from the working tree or from the index,
 * depending on the situation e.g. --cached/--index.  If we are
 * applying a non-git patch that incrementally updates the tree,
 * we read from the result of a previous diff.
 */
static int load_preimage(struct image *image,
			 struct patch *patch, struct stat *st,
			 const struct cache_entry *ce)
{
	struct strbuf buf = STRBUF_INIT;
	size_t len;
	char *img;
	struct patch *previous;
	int status;

	previous = previous_patch(patch, &status);
	if (status)
		return error(_("path %s has been renamed/deleted"),
			     patch->old_name);
	if (previous) {
		/* We have a patched copy in memory; use that. */
		strbuf_add(&buf, previous->result, previous->resultsize);
	} else {
		status = load_patch_target(&buf, ce, st,
					   patch->old_name, patch->old_mode);
		if (status < 0)
			return status;
		else if (status == SUBMODULE_PATCH_WITHOUT_INDEX) {
			/*
			 * There is no way to apply subproject
			 * patch without looking at the index.
			 * NEEDSWORK: shouldn't this be flagged
			 * as an error???
			 */
			free_fragment_list(patch->fragments);
			patch->fragments = NULL;
		} else if (status) {
			return error(_("read of %s failed"), patch->old_name);
		}
	}

	img = strbuf_detach(&buf, &len);
	prepare_image(image, img, len, !patch->is_binary);
	return 0;
}

static int three_way_merge(struct image *image,
			   char *path,
			   const unsigned char *base,
			   const unsigned char *ours,
			   const unsigned char *theirs)
{
	mmfile_t base_file, our_file, their_file;
	mmbuffer_t result = { NULL };
	int status;

	read_mmblob(&base_file, base);
	read_mmblob(&our_file, ours);
	read_mmblob(&their_file, theirs);
	status = ll_merge(&result, path,
			  &base_file, "base",
			  &our_file, "ours",
			  &their_file, "theirs", NULL);
	free(base_file.ptr);
	free(our_file.ptr);
	free(their_file.ptr);
	if (status < 0 || !result.ptr) {
		free(result.ptr);
		return -1;
	}
	clear_image(image);
	image->buf = result.ptr;
	image->len = result.size;

	return status;
}

/*
 * When directly falling back to add/add three-way merge, we read from
 * the current contents of the new_name.  In no cases other than that
 * this function will be called.
 */
static int load_current(struct image *image, struct patch *patch)
{
	struct strbuf buf = STRBUF_INIT;
	int status, pos;
	size_t len;
	char *img;
	struct stat st;
	struct cache_entry *ce;
	char *name = patch->new_name;
	unsigned mode = patch->new_mode;

	if (!patch->is_new)
		die("BUG: patch to %s is not a creation", patch->old_name);

	pos = cache_name_pos(name, strlen(name));
	if (pos < 0)
		return error(_("%s: does not exist in index"), name);
	ce = active_cache[pos];
	if (lstat(name, &st)) {
		if (errno != ENOENT)
			return error(_("%s: %s"), name, strerror(errno));
		if (checkout_target(&the_index, ce, &st))
			return -1;
	}
	if (verify_index_match(ce, &st))
		return error(_("%s: does not match index"), name);

	status = load_patch_target(&buf, ce, &st, name, mode);
	if (status < 0)
		return status;
	else if (status)
		return -1;
	img = strbuf_detach(&buf, &len);
	prepare_image(image, img, len, !patch->is_binary);
	return 0;
}

static int try_threeway(struct image *image, struct patch *patch,
			struct stat *st, const struct cache_entry *ce)
{
	unsigned char pre_sha1[20], post_sha1[20], our_sha1[20];
	struct strbuf buf = STRBUF_INIT;
	size_t len;
	int status;
	char *img;
	struct image tmp_image;

	/* No point falling back to 3-way merge in these cases */
	if (patch->is_delete ||
	    S_ISGITLINK(patch->old_mode) || S_ISGITLINK(patch->new_mode))
		return -1;

	/* Preimage the patch was prepared for */
	if (patch->is_new)
		write_sha1_file("", 0, blob_type, pre_sha1);
	else if (get_sha1(patch->old_sha1_prefix, pre_sha1) ||
		 read_blob_object(&buf, pre_sha1, patch->old_mode))
		return error("repository lacks the necessary blob to fall back on 3-way merge.");

	fprintf(stderr, "Falling back to three-way merge...\n");

	img = strbuf_detach(&buf, &len);
	prepare_image(&tmp_image, img, len, 1);
	/* Apply the patch to get the post image */
	if (apply_fragments(&tmp_image, patch) < 0) {
		clear_image(&tmp_image);
		return -1;
	}
	/* post_sha1[] is theirs */
	write_sha1_file(tmp_image.buf, tmp_image.len, blob_type, post_sha1);
	clear_image(&tmp_image);

	/* our_sha1[] is ours */
	if (patch->is_new) {
		if (load_current(&tmp_image, patch))
			return error("cannot read the current contents of '%s'",
				     patch->new_name);
	} else {
		if (load_preimage(&tmp_image, patch, st, ce))
			return error("cannot read the current contents of '%s'",
				     patch->old_name);
	}
	write_sha1_file(tmp_image.buf, tmp_image.len, blob_type, our_sha1);
	clear_image(&tmp_image);

	/* in-core three-way merge between post and our using pre as base */
	status = three_way_merge(image, patch->new_name,
				 pre_sha1, our_sha1, post_sha1);
	if (status < 0) {
		fprintf(stderr, "Failed to fall back on three-way merge...\n");
		return status;
	}

	if (status) {
		patch->conflicted_threeway = 1;
		if (patch->is_new)
			hashclr(patch->threeway_stage[0]);
		else
			hashcpy(patch->threeway_stage[0], pre_sha1);
		hashcpy(patch->threeway_stage[1], our_sha1);
		hashcpy(patch->threeway_stage[2], post_sha1);
		fprintf(stderr, "Applied patch to '%s' with conflicts.\n", patch->new_name);
	} else {
		fprintf(stderr, "Applied patch to '%s' cleanly.\n", patch->new_name);
	}
	return 0;
}

static int apply_data(struct patch *patch, struct stat *st, const struct cache_entry *ce)
{
	struct image image;

	if (load_preimage(&image, patch, st, ce) < 0)
		return -1;

	if (patch->direct_to_threeway ||
	    apply_fragments(&image, patch) < 0) {
		/* Note: with --reject, apply_fragments() returns 0 */
		if (!threeway || try_threeway(&image, patch, st, ce) < 0)
			return -1;
	}
	patch->result = image.buf;
	patch->resultsize = image.len;
	add_to_fn_table(patch);
	free(image.line_allocated);

	if (0 < patch->is_delete && patch->resultsize)
		return error(_("removal patch leaves file contents"));

	return 0;
}

/*
 * If "patch" that we are looking at modifies or deletes what we have,
 * we would want it not to lose any local modification we have, either
 * in the working tree or in the index.
 *
 * This also decides if a non-git patch is a creation patch or a
 * modification to an existing empty file.  We do not check the state
 * of the current tree for a creation patch in this function; the caller
 * check_patch() separately makes sure (and errors out otherwise) that
 * the path the patch creates does not exist in the current tree.
 */
static int check_preimage(struct patch *patch, struct cache_entry **ce, struct stat *st)
{
	const char *old_name = patch->old_name;
	struct patch *previous = NULL;
	int stat_ret = 0, status;
	unsigned st_mode = 0;

	if (!old_name)
		return 0;

	assert(patch->is_new <= 0);
	previous = previous_patch(patch, &status);

	if (status)
		return error(_("path %s has been renamed/deleted"), old_name);
	if (previous) {
		st_mode = previous->new_mode;
	} else if (!cached) {
		stat_ret = lstat(old_name, st);
		if (stat_ret && errno != ENOENT)
			return error(_("%s: %s"), old_name, strerror(errno));
	}

	if (check_index && !previous) {
		int pos = cache_name_pos(old_name, strlen(old_name));
		if (pos < 0) {
			if (patch->is_new < 0)
				goto is_new;
			return error(_("%s: does not exist in index"), old_name);
		}
		*ce = active_cache[pos];
		if (stat_ret < 0) {
			if (checkout_target(&the_index, *ce, st))
				return -1;
		}
		if (!cached && verify_index_match(*ce, st))
			return error(_("%s: does not match index"), old_name);
		if (cached)
			st_mode = (*ce)->ce_mode;
	} else if (stat_ret < 0) {
		if (patch->is_new < 0)
			goto is_new;
		return error(_("%s: %s"), old_name, strerror(errno));
	}

	if (!cached && !previous)
		st_mode = ce_mode_from_stat(*ce, st->st_mode);

	if (patch->is_new < 0)
		patch->is_new = 0;
	if (!patch->old_mode)
		patch->old_mode = st_mode;
	if ((st_mode ^ patch->old_mode) & S_IFMT)
		return error(_("%s: wrong type"), old_name);
	if (st_mode != patch->old_mode)
		warning(_("%s has type %o, expected %o"),
			old_name, st_mode, patch->old_mode);
	if (!patch->new_mode && !patch->is_delete)
		patch->new_mode = st_mode;
	return 0;

 is_new:
	patch->is_new = 1;
	patch->is_delete = 0;
	free(patch->old_name);
	patch->old_name = NULL;
	return 0;
}


#define EXISTS_IN_INDEX 1
#define EXISTS_IN_WORKTREE 2

static int check_to_create(const char *new_name, int ok_if_exists)
{
	struct stat nst;

	if (check_index &&
	    cache_name_pos(new_name, strlen(new_name)) >= 0 &&
	    !ok_if_exists)
		return EXISTS_IN_INDEX;
	if (cached)
		return 0;

	if (!lstat(new_name, &nst)) {
		if (S_ISDIR(nst.st_mode) || ok_if_exists)
			return 0;
		/*
		 * A leading component of new_name might be a symlink
		 * that is going to be removed with this patch, but
		 * still pointing at somewhere that has the path.
		 * In such a case, path "new_name" does not exist as
		 * far as git is concerned.
		 */
		if (has_symlink_leading_path(new_name, strlen(new_name)))
			return 0;

		return EXISTS_IN_WORKTREE;
	} else if ((errno != ENOENT) && (errno != ENOTDIR)) {
		return error("%s: %s", new_name, strerror(errno));
	}
	return 0;
}

/*
 * We need to keep track of how symlinks in the preimage are
 * manipulated by the patches.  A patch to add a/b/c where a/b
 * is a symlink should not be allowed to affect the directory
 * the symlink points at, but if the same patch removes a/b,
 * it is perfectly fine, as the patch removes a/b to make room
 * to create a directory a/b so that a/b/c can be created.
 */
static struct string_list symlink_changes;
#define SYMLINK_GOES_AWAY 01
#define SYMLINK_IN_RESULT 02

static uintptr_t register_symlink_changes(const char *path, uintptr_t what)
{
	struct string_list_item *ent;

	ent = string_list_lookup(&symlink_changes, path);
	if (!ent) {
		ent = string_list_insert(&symlink_changes, path);
		ent->util = (void *)0;
	}
	ent->util = (void *)(what | ((uintptr_t)ent->util));
	return (uintptr_t)ent->util;
}

static uintptr_t check_symlink_changes(const char *path)
{
	struct string_list_item *ent;

	ent = string_list_lookup(&symlink_changes, path);
	if (!ent)
		return 0;
	return (uintptr_t)ent->util;
}

static void prepare_symlink_changes(struct patch *patch)
{
	for ( ; patch; patch = patch->next) {
		if ((patch->old_name && S_ISLNK(patch->old_mode)) &&
		    (patch->is_rename || patch->is_delete))
			/* the symlink at patch->old_name is removed */
			register_symlink_changes(patch->old_name, SYMLINK_GOES_AWAY);

		if (patch->new_name && S_ISLNK(patch->new_mode))
			/* the symlink at patch->new_name is created or remains */
			register_symlink_changes(patch->new_name, SYMLINK_IN_RESULT);
	}
}

static int path_is_beyond_symlink_1(struct strbuf *name)
{
	do {
		unsigned int change;

		while (--name->len && name->buf[name->len] != '/')
			; /* scan backwards */
		if (!name->len)
			break;
		name->buf[name->len] = '\0';
		change = check_symlink_changes(name->buf);
		if (change & SYMLINK_IN_RESULT)
			return 1;
		if (change & SYMLINK_GOES_AWAY)
			/*
			 * This cannot be "return 0", because we may
			 * see a new one created at a higher level.
			 */
			continue;

		/* otherwise, check the preimage */
		if (check_index) {
			struct cache_entry *ce;

			ce = cache_file_exists(name->buf, name->len, ignore_case);
			if (ce && S_ISLNK(ce->ce_mode))
				return 1;
		} else {
			struct stat st;
			if (!lstat(name->buf, &st) && S_ISLNK(st.st_mode))
				return 1;
		}
	} while (1);
	return 0;
}

static int path_is_beyond_symlink(const char *name_)
{
	int ret;
	struct strbuf name = STRBUF_INIT;

	assert(*name_ != '\0');
	strbuf_addstr(&name, name_);
	ret = path_is_beyond_symlink_1(&name);
	strbuf_release(&name);

	return ret;
}

static void die_on_unsafe_path(struct patch *patch)
{
	const char *old_name = NULL;
	const char *new_name = NULL;
	if (patch->is_delete)
		old_name = patch->old_name;
	else if (!patch->is_new && !patch->is_copy)
		old_name = patch->old_name;
	if (!patch->is_delete)
		new_name = patch->new_name;

	if (old_name && !verify_path(old_name))
		die(_("invalid path '%s'"), old_name);
	if (new_name && !verify_path(new_name))
		die(_("invalid path '%s'"), new_name);
}

/*
 * Check and apply the patch in-core; leave the result in patch->result
 * for the caller to write it out to the final destination.
 */
static int check_patch(struct patch *patch)
{
	struct stat st;
	const char *old_name = patch->old_name;
	const char *new_name = patch->new_name;
	const char *name = old_name ? old_name : new_name;
	struct cache_entry *ce = NULL;
	struct patch *tpatch;
	int ok_if_exists;
	int status;

	patch->rejected = 1; /* we will drop this after we succeed */

	status = check_preimage(patch, &ce, &st);
	if (status)
		return status;
	old_name = patch->old_name;

	/*
	 * A type-change diff is always split into a patch to delete
	 * old, immediately followed by a patch to create new (see
	 * diff.c::run_diff()); in such a case it is Ok that the entry
	 * to be deleted by the previous patch is still in the working
	 * tree and in the index.
	 *
	 * A patch to swap-rename between A and B would first rename A
	 * to B and then rename B to A.  While applying the first one,
	 * the presence of B should not stop A from getting renamed to
	 * B; ask to_be_deleted() about the later rename.  Removal of
	 * B and rename from A to B is handled the same way by asking
	 * was_deleted().
	 */
	if ((tpatch = in_fn_table(new_name)) &&
	    (was_deleted(tpatch) || to_be_deleted(tpatch)))
		ok_if_exists = 1;
	else
		ok_if_exists = 0;

	if (new_name &&
	    ((0 < patch->is_new) || patch->is_rename || patch->is_copy)) {
		int err = check_to_create(new_name, ok_if_exists);

		if (err && threeway) {
			patch->direct_to_threeway = 1;
		} else switch (err) {
		case 0:
			break; /* happy */
		case EXISTS_IN_INDEX:
			return error(_("%s: already exists in index"), new_name);
			break;
		case EXISTS_IN_WORKTREE:
			return error(_("%s: already exists in working directory"),
				     new_name);
		default:
			return err;
		}

		if (!patch->new_mode) {
			if (0 < patch->is_new)
				patch->new_mode = S_IFREG | 0644;
			else
				patch->new_mode = patch->old_mode;
		}
	}

	if (new_name && old_name) {
		int same = !strcmp(old_name, new_name);
		if (!patch->new_mode)
			patch->new_mode = patch->old_mode;
		if ((patch->old_mode ^ patch->new_mode) & S_IFMT) {
			if (same)
				return error(_("new mode (%o) of %s does not "
					       "match old mode (%o)"),
					patch->new_mode, new_name,
					patch->old_mode);
			else
				return error(_("new mode (%o) of %s does not "
					       "match old mode (%o) of %s"),
					patch->new_mode, new_name,
					patch->old_mode, old_name);
		}
	}

	if (!unsafe_paths)
		die_on_unsafe_path(patch);

	/*
	 * An attempt to read from or delete a path that is beyond a
	 * symbolic link will be prevented by load_patch_target() that
	 * is called at the beginning of apply_data() so we do not
	 * have to worry about a patch marked with "is_delete" bit
	 * here.  We however need to make sure that the patch result
	 * is not deposited to a path that is beyond a symbolic link
	 * here.
	 */
	if (!patch->is_delete && path_is_beyond_symlink(patch->new_name))
		return error(_("affected file '%s' is beyond a symbolic link"),
			     patch->new_name);

	if (apply_data(patch, &st, ce) < 0)
		return error(_("%s: patch does not apply"), name);
	patch->rejected = 0;
	return 0;
}

static int check_patch_list(struct patch *patch)
{
	int err = 0;

	prepare_symlink_changes(patch);
	prepare_fn_table(patch);
	while (patch) {
		if (apply_verbosely)
			say_patch_name(stderr,
				       _("Checking patch %s..."), patch);
		err |= check_patch(patch);
		patch = patch->next;
	}
	return err;
}

/* This function tries to read the sha1 from the current index */
static int get_current_sha1(const char *path, unsigned char *sha1)
{
	int pos;

	if (read_cache() < 0)
		return -1;
	pos = cache_name_pos(path, strlen(path));
	if (pos < 0)
		return -1;
	hashcpy(sha1, active_cache[pos]->sha1);
	return 0;
}

static int preimage_sha1_in_gitlink_patch(struct patch *p, unsigned char sha1[20])
{
	/*
	 * A usable gitlink patch has only one fragment (hunk) that looks like:
	 * @@ -1 +1 @@
	 * -Subproject commit <old sha1>
	 * +Subproject commit <new sha1>
	 * or
	 * @@ -1 +0,0 @@
	 * -Subproject commit <old sha1>
	 * for a removal patch.
	 */
	struct fragment *hunk = p->fragments;
	static const char heading[] = "-Subproject commit ";
	char *preimage;

	if (/* does the patch have only one hunk? */
	    hunk && !hunk->next &&
	    /* is its preimage one line? */
	    hunk->oldpos == 1 && hunk->oldlines == 1 &&
	    /* does preimage begin with the heading? */
	    (preimage = memchr(hunk->patch, '\n', hunk->size)) != NULL &&
	    starts_with(++preimage, heading) &&
	    /* does it record full SHA-1? */
	    !get_sha1_hex(preimage + sizeof(heading) - 1, sha1) &&
	    preimage[sizeof(heading) + 40 - 1] == '\n' &&
	    /* does the abbreviated name on the index line agree with it? */
	    starts_with(preimage + sizeof(heading) - 1, p->old_sha1_prefix))
		return 0; /* it all looks fine */

	/* we may have full object name on the index line */
	return get_sha1_hex(p->old_sha1_prefix, sha1);
}

/* Build an index that contains the just the files needed for a 3way merge */
static void build_fake_ancestor(struct patch *list, const char *filename)
{
	struct patch *patch;
	struct index_state result = { NULL };
	static struct lock_file lock;

	/* Once we start supporting the reverse patch, it may be
	 * worth showing the new sha1 prefix, but until then...
	 */
	for (patch = list; patch; patch = patch->next) {
		unsigned char sha1[20];
		struct cache_entry *ce;
		const char *name;

		name = patch->old_name ? patch->old_name : patch->new_name;
		if (0 < patch->is_new)
			continue;

		if (S_ISGITLINK(patch->old_mode)) {
			if (!preimage_sha1_in_gitlink_patch(patch, sha1))
				; /* ok, the textual part looks sane */
			else
				die("sha1 information is lacking or useless for submodule %s",
				    name);
		} else if (!get_sha1_blob(patch->old_sha1_prefix, sha1)) {
			; /* ok */
		} else if (!patch->lines_added && !patch->lines_deleted) {
			/* mode-only change: update the current */
			if (get_current_sha1(patch->old_name, sha1))
				die("mode change for %s, which is not "
				    "in current HEAD", name);
		} else
			die("sha1 information is lacking or useless "
			    "(%s).", name);

		ce = make_cache_entry(patch->old_mode, sha1, name, 0, 0);
		if (!ce)
			die(_("make_cache_entry failed for path '%s'"), name);
		if (add_index_entry(&result, ce, ADD_CACHE_OK_TO_ADD))
			die ("Could not add %s to temporary index", name);
	}

	hold_lock_file_for_update(&lock, filename, LOCK_DIE_ON_ERROR);
	if (write_locked_index(&result, &lock, COMMIT_LOCK))
		die ("Could not write temporary index to %s", filename);

	discard_index(&result);
}

static void stat_patch_list(struct patch *patch)
{
	int files, adds, dels;

	for (files = adds = dels = 0 ; patch ; patch = patch->next) {
		files++;
		adds += patch->lines_added;
		dels += patch->lines_deleted;
		show_stats(patch);
	}

	print_stat_summary(stdout, files, adds, dels);
}

static void numstat_patch_list(struct patch *patch)
{
	for ( ; patch; patch = patch->next) {
		const char *name;
		name = patch->new_name ? patch->new_name : patch->old_name;
		if (patch->is_binary)
			printf("-\t-\t");
		else
			printf("%d\t%d\t", patch->lines_added, patch->lines_deleted);
		write_name_quoted(name, stdout, line_termination);
	}
}

static void show_file_mode_name(const char *newdelete, unsigned int mode, const char *name)
{
	if (mode)
		printf(" %s mode %06o %s\n", newdelete, mode, name);
	else
		printf(" %s %s\n", newdelete, name);
}

static void show_mode_change(struct patch *p, int show_name)
{
	if (p->old_mode && p->new_mode && p->old_mode != p->new_mode) {
		if (show_name)
			printf(" mode change %06o => %06o %s\n",
			       p->old_mode, p->new_mode, p->new_name);
		else
			printf(" mode change %06o => %06o\n",
			       p->old_mode, p->new_mode);
	}
}

static void show_rename_copy(struct patch *p)
{
	const char *renamecopy = p->is_rename ? "rename" : "copy";
	const char *old, *new;

	/* Find common prefix */
	old = p->old_name;
	new = p->new_name;
	while (1) {
		const char *slash_old, *slash_new;
		slash_old = strchr(old, '/');
		slash_new = strchr(new, '/');
		if (!slash_old ||
		    !slash_new ||
		    slash_old - old != slash_new - new ||
		    memcmp(old, new, slash_new - new))
			break;
		old = slash_old + 1;
		new = slash_new + 1;
	}
	/* p->old_name thru old is the common prefix, and old and new
	 * through the end of names are renames
	 */
	if (old != p->old_name)
		printf(" %s %.*s{%s => %s} (%d%%)\n", renamecopy,
		       (int)(old - p->old_name), p->old_name,
		       old, new, p->score);
	else
		printf(" %s %s => %s (%d%%)\n", renamecopy,
		       p->old_name, p->new_name, p->score);
	show_mode_change(p, 0);
}

static void summary_patch_list(struct patch *patch)
{
	struct patch *p;

	for (p = patch; p; p = p->next) {
		if (p->is_new)
			show_file_mode_name("create", p->new_mode, p->new_name);
		else if (p->is_delete)
			show_file_mode_name("delete", p->old_mode, p->old_name);
		else {
			if (p->is_rename || p->is_copy)
				show_rename_copy(p);
			else {
				if (p->score) {
					printf(" rewrite %s (%d%%)\n",
					       p->new_name, p->score);
					show_mode_change(p, 0);
				}
				else
					show_mode_change(p, 1);
			}
		}
	}
}

static void patch_stats(struct patch *patch)
{
	int lines = patch->lines_added + patch->lines_deleted;

	if (lines > max_change)
		max_change = lines;
	if (patch->old_name) {
		int len = quote_c_style(patch->old_name, NULL, NULL, 0);
		if (!len)
			len = strlen(patch->old_name);
		if (len > max_len)
			max_len = len;
	}
	if (patch->new_name) {
		int len = quote_c_style(patch->new_name, NULL, NULL, 0);
		if (!len)
			len = strlen(patch->new_name);
		if (len > max_len)
			max_len = len;
	}
}

static void remove_file(struct patch *patch, int rmdir_empty)
{
	if (update_index) {
		if (remove_file_from_cache(patch->old_name) < 0)
			die(_("unable to remove %s from index"), patch->old_name);
	}
	if (!cached) {
		if (!remove_or_warn(patch->old_mode, patch->old_name) && rmdir_empty) {
			remove_path(patch->old_name);
		}
	}
}

static void add_index_file(const char *path, unsigned mode, void *buf, unsigned long size)
{
	struct stat st;
	struct cache_entry *ce;
	int namelen = strlen(path);
	unsigned ce_size = cache_entry_size(namelen);

	if (!update_index)
		return;

	ce = xcalloc(1, ce_size);
	memcpy(ce->name, path, namelen);
	ce->ce_mode = create_ce_mode(mode);
	ce->ce_flags = create_ce_flags(0);
	ce->ce_namelen = namelen;
	if (S_ISGITLINK(mode)) {
		const char *s;

		if (!skip_prefix(buf, "Subproject commit ", &s) ||
		    get_sha1_hex(s, ce->sha1))
			die(_("corrupt patch for submodule %s"), path);
	} else {
		if (!cached) {
			if (lstat(path, &st) < 0)
				die_errno(_("unable to stat newly created file '%s'"),
					  path);
			fill_stat_cache_info(ce, &st);
		}
		if (write_sha1_file(buf, size, blob_type, ce->sha1) < 0)
			die(_("unable to create backing store for newly created file %s"), path);
	}
	if (add_cache_entry(ce, ADD_CACHE_OK_TO_ADD) < 0)
		die(_("unable to add cache entry for %s"), path);
}

static int try_create_file(const char *path, unsigned int mode, const char *buf, unsigned long size)
{
	int fd;
	struct strbuf nbuf = STRBUF_INIT;

	if (S_ISGITLINK(mode)) {
		struct stat st;
		if (!lstat(path, &st) && S_ISDIR(st.st_mode))
			return 0;
		return mkdir(path, 0777);
	}

	if (has_symlinks && S_ISLNK(mode))
		/* Although buf:size is counted string, it also is NUL
		 * terminated.
		 */
		return symlink(buf, path);

	fd = open(path, O_CREAT | O_EXCL | O_WRONLY, (mode & 0100) ? 0777 : 0666);
	if (fd < 0)
		return -1;

	if (convert_to_working_tree(path, buf, size, &nbuf)) {
		size = nbuf.len;
		buf  = nbuf.buf;
	}
	write_or_die(fd, buf, size);
	strbuf_release(&nbuf);

	if (close(fd) < 0)
		die_errno(_("closing file '%s'"), path);
	return 0;
}

/*
 * We optimistically assume that the directories exist,
 * which is true 99% of the time anyway. If they don't,
 * we create them and try again.
 */
static void create_one_file(char *path, unsigned mode, const char *buf, unsigned long size)
{
	if (cached)
		return;
	if (!try_create_file(path, mode, buf, size))
		return;

	if (errno == ENOENT) {
		if (safe_create_leading_directories(path))
			return;
		if (!try_create_file(path, mode, buf, size))
			return;
	}

	if (errno == EEXIST || errno == EACCES) {
		/* We may be trying to create a file where a directory
		 * used to be.
		 */
		struct stat st;
		if (!lstat(path, &st) && (!S_ISDIR(st.st_mode) || !rmdir(path)))
			errno = EEXIST;
	}

	if (errno == EEXIST) {
		unsigned int nr = getpid();

		for (;;) {
			char newpath[PATH_MAX];
			mksnpath(newpath, sizeof(newpath), "%s~%u", path, nr);
			if (!try_create_file(newpath, mode, buf, size)) {
				if (!rename(newpath, path))
					return;
				unlink_or_warn(newpath);
				break;
			}
			if (errno != EEXIST)
				break;
			++nr;
		}
	}
	die_errno(_("unable to write file '%s' mode %o"), path, mode);
}

static void add_conflicted_stages_file(struct patch *patch)
{
	int stage, namelen;
	unsigned ce_size, mode;
	struct cache_entry *ce;

	if (!update_index)
		return;
	namelen = strlen(patch->new_name);
	ce_size = cache_entry_size(namelen);
	mode = patch->new_mode ? patch->new_mode : (S_IFREG | 0644);

	remove_file_from_cache(patch->new_name);
	for (stage = 1; stage < 4; stage++) {
		if (is_null_sha1(patch->threeway_stage[stage - 1]))
			continue;
		ce = xcalloc(1, ce_size);
		memcpy(ce->name, patch->new_name, namelen);
		ce->ce_mode = create_ce_mode(mode);
		ce->ce_flags = create_ce_flags(stage);
		ce->ce_namelen = namelen;
		hashcpy(ce->sha1, patch->threeway_stage[stage - 1]);
		if (add_cache_entry(ce, ADD_CACHE_OK_TO_ADD) < 0)
			die(_("unable to add cache entry for %s"), patch->new_name);
	}
}

static void create_file(struct patch *patch)
{
	char *path = patch->new_name;
	unsigned mode = patch->new_mode;
	unsigned long size = patch->resultsize;
	char *buf = patch->result;

	if (!mode)
		mode = S_IFREG | 0644;
	create_one_file(path, mode, buf, size);

	if (patch->conflicted_threeway)
		add_conflicted_stages_file(patch);
	else
		add_index_file(path, mode, buf, size);
}

/* phase zero is to remove, phase one is to create */
static void write_out_one_result(struct patch *patch, int phase)
{
	if (patch->is_delete > 0) {
		if (phase == 0)
			remove_file(patch, 1);
		return;
	}
	if (patch->is_new > 0 || patch->is_copy) {
		if (phase == 1)
			create_file(patch);
		return;
	}
	/*
	 * Rename or modification boils down to the same
	 * thing: remove the old, write the new
	 */
	if (phase == 0)
		remove_file(patch, patch->is_rename);
	if (phase == 1)
		create_file(patch);
}

static int write_out_one_reject(struct patch *patch)
{
	FILE *rej;
	char namebuf[PATH_MAX];
	struct fragment *frag;
	int cnt = 0;
	struct strbuf sb = STRBUF_INIT;

	for (cnt = 0, frag = patch->fragments; frag; frag = frag->next) {
		if (!frag->rejected)
			continue;
		cnt++;
	}

	if (!cnt) {
		if (apply_verbosely)
			say_patch_name(stderr,
				       _("Applied patch %s cleanly."), patch);
		return 0;
	}

	/* This should not happen, because a removal patch that leaves
	 * contents are marked "rejected" at the patch level.
	 */
	if (!patch->new_name)
		die(_("internal error"));

	/* Say this even without --verbose */
	strbuf_addf(&sb, Q_("Applying patch %%s with %d reject...",
			    "Applying patch %%s with %d rejects...",
			    cnt),
		    cnt);
	say_patch_name(stderr, sb.buf, patch);
	strbuf_release(&sb);

	cnt = strlen(patch->new_name);
	if (ARRAY_SIZE(namebuf) <= cnt + 5) {
		cnt = ARRAY_SIZE(namebuf) - 5;
		warning(_("truncating .rej filename to %.*s.rej"),
			cnt - 1, patch->new_name);
	}
	memcpy(namebuf, patch->new_name, cnt);
	memcpy(namebuf + cnt, ".rej", 5);

	rej = fopen(namebuf, "w");
	if (!rej)
		return error(_("cannot open %s: %s"), namebuf, strerror(errno));

	/* Normal git tools never deal with .rej, so do not pretend
	 * this is a git patch by saying --git or giving extended
	 * headers.  While at it, maybe please "kompare" that wants
	 * the trailing TAB and some garbage at the end of line ;-).
	 */
	fprintf(rej, "diff a/%s b/%s\t(rejected hunks)\n",
		patch->new_name, patch->new_name);
	for (cnt = 1, frag = patch->fragments;
	     frag;
	     cnt++, frag = frag->next) {
		if (!frag->rejected) {
			fprintf_ln(stderr, _("Hunk #%d applied cleanly."), cnt);
			continue;
		}
		fprintf_ln(stderr, _("Rejected hunk #%d."), cnt);
		fprintf(rej, "%.*s", frag->size, frag->patch);
		if (frag->patch[frag->size-1] != '\n')
			fputc('\n', rej);
	}
	fclose(rej);
	return -1;
}

static int write_out_results(struct patch *list)
{
	int phase;
	int errs = 0;
	struct patch *l;
	struct string_list cpath = STRING_LIST_INIT_DUP;

	for (phase = 0; phase < 2; phase++) {
		l = list;
		while (l) {
			if (l->rejected)
				errs = 1;
			else {
				write_out_one_result(l, phase);
				if (phase == 1) {
					if (write_out_one_reject(l))
						errs = 1;
					if (l->conflicted_threeway) {
						string_list_append(&cpath, l->new_name);
						errs = 1;
					}
				}
			}
			l = l->next;
		}
	}

	if (cpath.nr) {
		struct string_list_item *item;

		string_list_sort(&cpath);
		for_each_string_list_item(item, &cpath)
			fprintf(stderr, "U %s\n", item->string);
		string_list_clear(&cpath, 0);

		rerere(0);
	}

	return errs;
}

static struct lock_file lock_file;

#define INACCURATE_EOF	(1<<0)
#define RECOUNT		(1<<1)

static int apply_patch(int fd, const char *filename, int options)
{
	size_t offset;
	struct strbuf buf = STRBUF_INIT; /* owns the patch text */
	struct patch *list = NULL, **listp = &list;
	int skipped_patch = 0;

	patch_input_file = filename;
	read_patch_file(&buf, fd);
	offset = 0;
	while (offset < buf.len) {
		struct patch *patch;
		int nr;

		patch = xcalloc(1, sizeof(*patch));
		patch->inaccurate_eof = !!(options & INACCURATE_EOF);
		patch->recount =  !!(options & RECOUNT);
		nr = parse_chunk(buf.buf + offset, buf.len - offset, patch);
		if (nr < 0)
			break;
		if (apply_in_reverse)
			reverse_patches(patch);
		if (use_patch(patch)) {
			patch_stats(patch);
			*listp = patch;
			listp = &patch->next;
		}
		else {
			free_patch(patch);
			skipped_patch++;
		}
		offset += nr;
	}

	if (!list && !skipped_patch)
		die(_("unrecognized input"));

	if (whitespace_error && (ws_error_action == die_on_ws_error))
		apply = 0;

	update_index = check_index && apply;
	if (update_index && newfd < 0)
		newfd = hold_locked_index(&lock_file, 1);

	if (check_index) {
		if (read_cache() < 0)
			die(_("unable to read index file"));
	}

	if ((check || apply) &&
	    check_patch_list(list) < 0 &&
	    !apply_with_reject)
		exit(1);

	if (apply && write_out_results(list)) {
		if (apply_with_reject)
			exit(1);
		/* with --3way, we still need to write the index out */
		return 1;
	}

	if (fake_ancestor)
		build_fake_ancestor(list, fake_ancestor);

	if (diffstat)
		stat_patch_list(list);

	if (numstat)
		numstat_patch_list(list);

	if (summary)
		summary_patch_list(list);

	free_patch_list(list);
	strbuf_release(&buf);
	string_list_clear(&fn_table, 0);
	return 0;
}

static void git_apply_config(void)
{
	git_config_get_string_const("apply.whitespace", &apply_default_whitespace);
	git_config_get_string_const("apply.ignorewhitespace", &apply_default_ignorewhitespace);
	git_config(git_default_config, NULL);
}

static int option_parse_exclude(const struct option *opt,
				const char *arg, int unset)
{
	add_name_limit(arg, 1);
	return 0;
}

static int option_parse_include(const struct option *opt,
				const char *arg, int unset)
{
	add_name_limit(arg, 0);
	has_include = 1;
	return 0;
}

static int option_parse_p(const struct option *opt,
			  const char *arg, int unset)
{
	p_value = atoi(arg);
	p_value_known = 1;
	return 0;
}

static int option_parse_z(const struct option *opt,
			  const char *arg, int unset)
{
	if (unset)
		line_termination = '\n';
	else
		line_termination = 0;
	return 0;
}

static int option_parse_space_change(const struct option *opt,
			  const char *arg, int unset)
{
	if (unset)
		ws_ignore_action = ignore_ws_none;
	else
		ws_ignore_action = ignore_ws_change;
	return 0;
}

static int option_parse_whitespace(const struct option *opt,
				   const char *arg, int unset)
{
	const char **whitespace_option = opt->value;

	*whitespace_option = arg;
	parse_whitespace_option(arg);
	return 0;
}

static int option_parse_directory(const struct option *opt,
				  const char *arg, int unset)
{
	root_len = strlen(arg);
	if (root_len && arg[root_len - 1] != '/') {
		char *new_root;
		root = new_root = xmalloc(root_len + 2);
		strcpy(new_root, arg);
		strcpy(new_root + root_len++, "/");
	} else
		root = arg;
	return 0;
}

int cmd_apply(int argc, const char **argv, const char *prefix_)
{
	int i;
	int errs = 0;
	int is_not_gitdir = !startup_info->have_repository;
	int force_apply = 0;

	const char *whitespace_option = NULL;

	struct option builtin_apply_options[] = {
		{ OPTION_CALLBACK, 0, "exclude", NULL, N_("path"),
			N_("don't apply changes matching the given path"),
			0, option_parse_exclude },
		{ OPTION_CALLBACK, 0, "include", NULL, N_("path"),
			N_("apply changes matching the given path"),
			0, option_parse_include },
		{ OPTION_CALLBACK, 'p', NULL, NULL, N_("num"),
			N_("remove <num> leading slashes from traditional diff paths"),
			0, option_parse_p },
		OPT_BOOL(0, "no-add", &no_add,
			N_("ignore additions made by the patch")),
		OPT_BOOL(0, "stat", &diffstat,
			N_("instead of applying the patch, output diffstat for the input")),
		OPT_NOOP_NOARG(0, "allow-binary-replacement"),
		OPT_NOOP_NOARG(0, "binary"),
		OPT_BOOL(0, "numstat", &numstat,
			N_("show number of added and deleted lines in decimal notation")),
		OPT_BOOL(0, "summary", &summary,
			N_("instead of applying the patch, output a summary for the input")),
		OPT_BOOL(0, "check", &check,
			N_("instead of applying the patch, see if the patch is applicable")),
		OPT_BOOL(0, "index", &check_index,
			N_("make sure the patch is applicable to the current index")),
		OPT_BOOL(0, "cached", &cached,
			N_("apply a patch without touching the working tree")),
		OPT_BOOL(0, "unsafe-paths", &unsafe_paths,
			N_("accept a patch that touches outside the working area")),
		OPT_BOOL(0, "apply", &force_apply,
			N_("also apply the patch (use with --stat/--summary/--check)")),
		OPT_BOOL('3', "3way", &threeway,
			 N_( "attempt three-way merge if a patch does not apply")),
		OPT_FILENAME(0, "build-fake-ancestor", &fake_ancestor,
			N_("build a temporary index based on embedded index information")),
		{ OPTION_CALLBACK, 'z', NULL, NULL, NULL,
			N_("paths are separated with NUL character"),
			PARSE_OPT_NOARG, option_parse_z },
		OPT_INTEGER('C', NULL, &p_context,
				N_("ensure at least <n> lines of context match")),
		{ OPTION_CALLBACK, 0, "whitespace", &whitespace_option, N_("action"),
			N_("detect new or modified lines that have whitespace errors"),
			0, option_parse_whitespace },
		{ OPTION_CALLBACK, 0, "ignore-space-change", NULL, NULL,
			N_("ignore changes in whitespace when finding context"),
			PARSE_OPT_NOARG, option_parse_space_change },
		{ OPTION_CALLBACK, 0, "ignore-whitespace", NULL, NULL,
			N_("ignore changes in whitespace when finding context"),
			PARSE_OPT_NOARG, option_parse_space_change },
		OPT_BOOL('R', "reverse", &apply_in_reverse,
			N_("apply the patch in reverse")),
		OPT_BOOL(0, "unidiff-zero", &unidiff_zero,
			N_("don't expect at least one line of context")),
		OPT_BOOL(0, "reject", &apply_with_reject,
			N_("leave the rejected hunks in corresponding *.rej files")),
		OPT_BOOL(0, "allow-overlap", &allow_overlap,
			N_("allow overlapping hunks")),
		OPT__VERBOSE(&apply_verbosely, N_("be verbose")),
		OPT_BIT(0, "inaccurate-eof", &options,
			N_("tolerate incorrectly detected missing new-line at the end of file"),
			INACCURATE_EOF),
		OPT_BIT(0, "recount", &options,
			N_("do not trust the line counts in the hunk headers"),
			RECOUNT),
		{ OPTION_CALLBACK, 0, "directory", NULL, N_("root"),
			N_("prepend <root> to all filenames"),
			0, option_parse_directory },
		{ OPTION_CALLBACK, 0, "recurse-submodules", &recurse_submodules,
		  "checkout", "control recursive updating of submodules",
		  PARSE_OPT_OPTARG, option_parse_update_submodules },
		{ OPTION_STRING, 0, "recurse-submodules-default",
		  &recurse_submodules_default, NULL,
		  "default mode for recursion", PARSE_OPT_HIDDEN },
		OPT_END()
	};

	prefix = prefix_;
	prefix_length = prefix ? strlen(prefix) : 0;
	gitmodules_config();
	git_apply_config();
	if (apply_default_whitespace)
		parse_whitespace_option(apply_default_whitespace);
	if (apply_default_ignorewhitespace)
		parse_ignorewhitespace_option(apply_default_ignorewhitespace);

	argc = parse_options(argc, argv, prefix, builtin_apply_options,
			apply_usage, 0);

	set_config_update_recurse_submodules(
		parse_update_recurse_submodules_arg("--recurse-submodules-default",
						    recurse_submodules_default),
		recurse_submodules);
	if (apply_with_reject && threeway)
		die("--reject and --3way cannot be used together.");
	if (cached && threeway)
		die("--cached and --3way cannot be used together.");
	if (threeway) {
		if (is_not_gitdir)
			die(_("--3way outside a repository"));
		check_index = 1;
	}
	if (apply_with_reject)
		apply = apply_verbosely = 1;
	if (!force_apply && (diffstat || numstat || summary || check || fake_ancestor))
		apply = 0;
	if (check_index && is_not_gitdir)
		die(_("--index outside a repository"));
	if (cached) {
		if (is_not_gitdir)
			die(_("--cached outside a repository"));
		check_index = 1;
	}
	if (check_index)
		unsafe_paths = 0;

	for (i = 0; i < argc; i++) {
		const char *arg = argv[i];
		int fd;

		if (!strcmp(arg, "-")) {
			errs |= apply_patch(0, "<stdin>", options);
			read_stdin = 0;
			continue;
		} else if (0 < prefix_length)
			arg = prefix_filename(prefix, prefix_length, arg);

		fd = open(arg, O_RDONLY);
		if (fd < 0)
			die_errno(_("can't open patch '%s'"), arg);
		read_stdin = 0;
		set_default_whitespace_mode(whitespace_option);
		errs |= apply_patch(fd, arg, options);
		close(fd);
	}
	set_default_whitespace_mode(whitespace_option);
	if (read_stdin)
		errs |= apply_patch(0, "<stdin>", options);
	if (whitespace_error) {
		if (squelch_whitespace_errors &&
		    squelch_whitespace_errors < whitespace_error) {
			int squelched =
				whitespace_error - squelch_whitespace_errors;
			warning(Q_("squelched %d whitespace error",
				   "squelched %d whitespace errors",
				   squelched),
				squelched);
		}
		if (ws_error_action == die_on_ws_error)
			die(Q_("%d line adds whitespace errors.",
			       "%d lines add whitespace errors.",
			       whitespace_error),
			    whitespace_error);
		if (applied_after_fixing_ws && apply)
			warning("%d line%s applied after"
				" fixing whitespace errors.",
				applied_after_fixing_ws,
				applied_after_fixing_ws == 1 ? "" : "s");
		else if (whitespace_error)
			warning(Q_("%d line adds whitespace errors.",
				   "%d lines add whitespace errors.",
				   whitespace_error),
				whitespace_error);
	}

	if (update_index) {
		if (write_locked_index(&the_index, &lock_file, COMMIT_LOCK))
			die(_("Unable to write new index file"));
	}

	return !!errs;
}
