#include "cache.h"
#include "wt-status.h"
#include "object.h"
#include "dir.h"
#include "commit.h"
#include "diff.h"
#include "revision.h"
#include "diffcore.h"
#include "quote.h"
#include "run-command.h"
#include "remote.h"

static char default_wt_status_colors[][COLOR_MAXLEN] = {
	GIT_COLOR_NORMAL, /* WT_STATUS_HEADER */
	GIT_COLOR_GREEN,  /* WT_STATUS_UPDATED */
	GIT_COLOR_RED,    /* WT_STATUS_CHANGED */
	GIT_COLOR_RED,    /* WT_STATUS_UNTRACKED */
	GIT_COLOR_RED,    /* WT_STATUS_NOBRANCH */
	GIT_COLOR_RED,    /* WT_STATUS_UNMERGED */
};

static const char *color(int slot, struct wt_status *s)
{
	return s->use_color > 0 ? s->color_palette[slot] : "";
}

void wt_status_prepare(struct wt_status *s)
{
	unsigned char sha1[20];
	const char *head;

	memset(s, 0, sizeof(*s));
	memcpy(s->color_palette, default_wt_status_colors,
	       sizeof(default_wt_status_colors));
	s->show_untracked_files = SHOW_NORMAL_UNTRACKED_FILES;
	s->use_color = -1;
	s->relative_paths = 1;
	head = resolve_ref("HEAD", sha1, 0, NULL);
	s->branch = head ? xstrdup(head) : NULL;
	s->reference = "HEAD";
	s->fp = stdout;
	s->index_file = get_index_file();
	s->change.strdup_strings = 1;
	s->untracked.strdup_strings = 1;
}

static void wt_status_print_unmerged_header(struct wt_status *s)
{
	const char *c = color(WT_STATUS_HEADER, s);

	color_fprintf_ln(s->fp, c, "# Unmerged paths:");
	if (!advice_status_hints)
		return;
	if (s->in_merge)
		;
	else if (!s->is_initial)
		color_fprintf_ln(s->fp, c, "#   (use \"git reset %s <file>...\" to unstage)", s->reference);
	else
		color_fprintf_ln(s->fp, c, "#   (use \"git rm --cached <file>...\" to unstage)");
	color_fprintf_ln(s->fp, c, "#   (use \"git add/rm <file>...\" as appropriate to mark resolution)");
	color_fprintf_ln(s->fp, c, "#");
}

static void wt_status_print_cached_header(struct wt_status *s)
{
	const char *c = color(WT_STATUS_HEADER, s);

	color_fprintf_ln(s->fp, c, "# Changes to be committed:");
	if (!advice_status_hints)
		return;
	if (s->in_merge)
		; /* NEEDSWORK: use "git reset --unresolve"??? */
	else if (!s->is_initial)
		color_fprintf_ln(s->fp, c, "#   (use \"git reset %s <file>...\" to unstage)", s->reference);
	else
		color_fprintf_ln(s->fp, c, "#   (use \"git rm --cached <file>...\" to unstage)");
	color_fprintf_ln(s->fp, c, "#");
}

static void wt_status_print_dirty_header(struct wt_status *s,
					 int has_deleted)
{
	const char *c = color(WT_STATUS_HEADER, s);

	color_fprintf_ln(s->fp, c, "# Changed but not updated:");
	if (!advice_status_hints)
		return;
	if (!has_deleted)
		color_fprintf_ln(s->fp, c, "#   (use \"git add <file>...\" to update what will be committed)");
	else
		color_fprintf_ln(s->fp, c, "#   (use \"git add/rm <file>...\" to update what will be committed)");
	color_fprintf_ln(s->fp, c, "#   (use \"git checkout -- <file>...\" to discard changes in working directory)");
	color_fprintf_ln(s->fp, c, "#");
}

static void wt_status_print_untracked_header(struct wt_status *s)
{
	const char *c = color(WT_STATUS_HEADER, s);
	color_fprintf_ln(s->fp, c, "# Untracked files:");
	if (!advice_status_hints)
		return;
	color_fprintf_ln(s->fp, c, "#   (use \"git add <file>...\" to include in what will be committed)");
	color_fprintf_ln(s->fp, c, "#");
}

static void wt_status_print_trailer(struct wt_status *s)
{
	color_fprintf_ln(s->fp, color(WT_STATUS_HEADER, s), "#");
}

#define quote_path quote_path_relative

static void wt_status_print_unmerged_data(struct wt_status *s,
					  struct string_list_item *it)
{
	const char *c = color(WT_STATUS_UNMERGED, s);
	struct wt_status_change_data *d = it->util;
	struct strbuf onebuf = STRBUF_INIT;
	const char *one, *how = "bug";

	one = quote_path(it->string, -1, &onebuf, s->prefix);
	color_fprintf(s->fp, color(WT_STATUS_HEADER, s), "#\t");
	switch (d->stagemask) {
	case 1: how = "both deleted:"; break;
	case 2: how = "added by us:"; break;
	case 3: how = "deleted by them:"; break;
	case 4: how = "added by them:"; break;
	case 5: how = "deleted by us:"; break;
	case 6: how = "both added:"; break;
	case 7: how = "both modified:"; break;
	}
	color_fprintf(s->fp, c, "%-20s%s\n", how, one);
	strbuf_release(&onebuf);
}

static void wt_status_print_change_data(struct wt_status *s,
					int change_type,
					struct string_list_item *it)
{
	struct wt_status_change_data *d = it->util;
	const char *c = color(change_type, s);
	int status = status;
	char *one_name;
	char *two_name;
	const char *one, *two;
	struct strbuf onebuf = STRBUF_INIT, twobuf = STRBUF_INIT;

	one_name = two_name = it->string;
	switch (change_type) {
	case WT_STATUS_UPDATED:
		status = d->index_status;
		if (d->head_path)
			one_name = d->head_path;
		break;
	case WT_STATUS_CHANGED:
		status = d->worktree_status;
		break;
	}

	one = quote_path(one_name, -1, &onebuf, s->prefix);
	two = quote_path(two_name, -1, &twobuf, s->prefix);

	color_fprintf(s->fp, color(WT_STATUS_HEADER, s), "#\t");
	switch (status) {
	case DIFF_STATUS_ADDED:
		color_fprintf(s->fp, c, "new file:   %s", one);
		break;
	case DIFF_STATUS_COPIED:
		color_fprintf(s->fp, c, "copied:     %s -> %s", one, two);
		break;
	case DIFF_STATUS_DELETED:
		color_fprintf(s->fp, c, "deleted:    %s", one);
		break;
	case DIFF_STATUS_MODIFIED:
		color_fprintf(s->fp, c, "modified:   %s", one);
		break;
	case DIFF_STATUS_RENAMED:
		color_fprintf(s->fp, c, "renamed:    %s -> %s", one, two);
		break;
	case DIFF_STATUS_TYPE_CHANGED:
		color_fprintf(s->fp, c, "typechange: %s", one);
		break;
	case DIFF_STATUS_UNKNOWN:
		color_fprintf(s->fp, c, "unknown:    %s", one);
		break;
	case DIFF_STATUS_UNMERGED:
		color_fprintf(s->fp, c, "unmerged:   %s", one);
		break;
	default:
		die("bug: unhandled diff status %c", status);
	}
	fprintf(s->fp, "\n");
	strbuf_release(&onebuf);
	strbuf_release(&twobuf);
}

static void wt_status_collect_changed_cb(struct diff_queue_struct *q,
					 struct diff_options *options,
					 void *data)
{
	struct wt_status *s = data;
	int i;

	if (!q->nr)
		return;
	s->workdir_dirty = 1;
	for (i = 0; i < q->nr; i++) {
		struct diff_filepair *p;
		struct string_list_item *it;
		struct wt_status_change_data *d;

		p = q->queue[i];
		it = string_list_insert(p->one->path, &s->change);
		d = it->util;
		if (!d) {
			d = xcalloc(1, sizeof(*d));
			it->util = d;
		}
		if (!d->worktree_status)
			d->worktree_status = p->status;
	}
}

static int unmerged_mask(const char *path)
{
	int pos, mask;
	struct cache_entry *ce;

	pos = cache_name_pos(path, strlen(path));
	if (0 <= pos)
		return 0;

	mask = 0;
	pos = -pos-1;
	while (pos < active_nr) {
		ce = active_cache[pos++];
		if (strcmp(ce->name, path) || !ce_stage(ce))
			break;
		mask |= (1 << (ce_stage(ce) - 1));
	}
	return mask;
}

static void wt_status_collect_updated_cb(struct diff_queue_struct *q,
					 struct diff_options *options,
					 void *data)
{
	struct wt_status *s = data;
	int i;

	for (i = 0; i < q->nr; i++) {
		struct diff_filepair *p;
		struct string_list_item *it;
		struct wt_status_change_data *d;

		p = q->queue[i];
		it = string_list_insert(p->two->path, &s->change);
		d = it->util;
		if (!d) {
			d = xcalloc(1, sizeof(*d));
			it->util = d;
		}
		if (!d->index_status)
			d->index_status = p->status;
		switch (p->status) {
		case DIFF_STATUS_COPIED:
		case DIFF_STATUS_RENAMED:
			d->head_path = xstrdup(p->one->path);
			break;
		case DIFF_STATUS_UNMERGED:
			d->stagemask = unmerged_mask(p->two->path);
			break;
		}
	}
}

static void wt_status_collect_changes_worktree(struct wt_status *s)
{
	struct rev_info rev;

	init_revisions(&rev, NULL);
	setup_revisions(0, NULL, &rev, NULL);
	rev.diffopt.output_format |= DIFF_FORMAT_CALLBACK;
	rev.diffopt.format_callback = wt_status_collect_changed_cb;
	rev.diffopt.format_callback_data = s;
	rev.prune_data = s->pathspec;
	run_diff_files(&rev, 0);
}

static void wt_status_collect_changes_index(struct wt_status *s)
{
	struct rev_info rev;

	init_revisions(&rev, NULL);
	setup_revisions(0, NULL, &rev,
		s->is_initial ? EMPTY_TREE_SHA1_HEX : s->reference);
	rev.diffopt.output_format |= DIFF_FORMAT_CALLBACK;
	rev.diffopt.format_callback = wt_status_collect_updated_cb;
	rev.diffopt.format_callback_data = s;
	rev.diffopt.detect_rename = 1;
	rev.diffopt.rename_limit = 200;
	rev.diffopt.break_opt = 0;
	rev.prune_data = s->pathspec;
	run_diff_index(&rev, 1);
}

static void wt_status_collect_changes_initial(struct wt_status *s)
{
	int i;

	for (i = 0; i < active_nr; i++) {
		struct string_list_item *it;
		struct wt_status_change_data *d;
		struct cache_entry *ce = active_cache[i];

		if (!ce_path_match(ce, s->pathspec))
			continue;
		it = string_list_insert(ce->name, &s->change);
		d = it->util;
		if (!d) {
			d = xcalloc(1, sizeof(*d));
			it->util = d;
		}
		if (ce_stage(ce)) {
			d->index_status = DIFF_STATUS_UNMERGED;
			d->stagemask |= (1 << (ce_stage(ce) - 1));
		}
		else
			d->index_status = DIFF_STATUS_ADDED;
	}
}

static void wt_status_collect_untracked(struct wt_status *s)
{
	int i;
	struct dir_struct dir;

	if (!s->show_untracked_files)
		return;
	memset(&dir, 0, sizeof(dir));
	if (s->show_untracked_files != SHOW_ALL_UNTRACKED_FILES)
		dir.flags |=
			DIR_SHOW_OTHER_DIRECTORIES | DIR_HIDE_EMPTY_DIRECTORIES;
	setup_standard_excludes(&dir);

	fill_directory(&dir, s->pathspec);
	for (i = 0; i < dir.nr; i++) {
		struct dir_entry *ent = dir.entries[i];
		if (!cache_name_is_other(ent->name, ent->len))
			continue;
		if (!match_pathspec(s->pathspec, ent->name, ent->len, 0, NULL))
			continue;
		s->workdir_untracked = 1;
		string_list_insert(ent->name, &s->untracked);
	}
}

void wt_status_collect(struct wt_status *s)
{
	wt_status_collect_changes_worktree(s);

	if (s->is_initial)
		wt_status_collect_changes_initial(s);
	else
		wt_status_collect_changes_index(s);
	wt_status_collect_untracked(s);
}

static void wt_status_print_unmerged(struct wt_status *s)
{
	int shown_header = 0;
	int i;

	for (i = 0; i < s->change.nr; i++) {
		struct wt_status_change_data *d;
		struct string_list_item *it;
		it = &(s->change.items[i]);
		d = it->util;
		if (!d->stagemask)
			continue;
		if (!shown_header) {
			wt_status_print_unmerged_header(s);
			shown_header = 1;
		}
		wt_status_print_unmerged_data(s, it);
	}
	if (shown_header)
		wt_status_print_trailer(s);

}

static void wt_status_print_updated(struct wt_status *s)
{
	int shown_header = 0;
	int i;

	for (i = 0; i < s->change.nr; i++) {
		struct wt_status_change_data *d;
		struct string_list_item *it;
		it = &(s->change.items[i]);
		d = it->util;
		if (!d->index_status ||
		    d->index_status == DIFF_STATUS_UNMERGED)
			continue;
		if (!shown_header) {
			wt_status_print_cached_header(s);
			s->commitable = 1;
			shown_header = 1;
		}
		wt_status_print_change_data(s, WT_STATUS_UPDATED, it);
	}
	if (shown_header)
		wt_status_print_trailer(s);
}

/*
 * -1 : has delete
 *  0 : no change
 *  1 : some change but no delete
 */
static int wt_status_check_worktree_changes(struct wt_status *s)
{
	int i;
	int changes = 0;

	for (i = 0; i < s->change.nr; i++) {
		struct wt_status_change_data *d;
		d = s->change.items[i].util;
		if (!d->worktree_status ||
		    d->worktree_status == DIFF_STATUS_UNMERGED)
			continue;
		changes = 1;
		if (d->worktree_status == DIFF_STATUS_DELETED)
			return -1;
	}
	return changes;
}

static void wt_status_print_changed(struct wt_status *s)
{
	int i;
	int worktree_changes = wt_status_check_worktree_changes(s);

	if (!worktree_changes)
		return;

	wt_status_print_dirty_header(s, worktree_changes < 0);

	for (i = 0; i < s->change.nr; i++) {
		struct wt_status_change_data *d;
		struct string_list_item *it;
		it = &(s->change.items[i]);
		d = it->util;
		if (!d->worktree_status ||
		    d->worktree_status == DIFF_STATUS_UNMERGED)
			continue;
		wt_status_print_change_data(s, WT_STATUS_CHANGED, it);
	}
	wt_status_print_trailer(s);
}

static void wt_status_print_submodule_summary(struct wt_status *s)
{
	struct child_process sm_summary;
	char summary_limit[64];
	char index[PATH_MAX];
	const char *env[] = { index, NULL };
	const char *argv[] = {
		"submodule",
		"summary",
		"--cached",
		"--for-status",
		"--summary-limit",
		summary_limit,
		s->amend ? "HEAD^" : "HEAD",
		NULL
	};

	sprintf(summary_limit, "%d", s->submodule_summary);
	snprintf(index, sizeof(index), "GIT_INDEX_FILE=%s", s->index_file);

	memset(&sm_summary, 0, sizeof(sm_summary));
	sm_summary.argv = argv;
	sm_summary.env = env;
	sm_summary.git_cmd = 1;
	sm_summary.no_stdin = 1;
	fflush(s->fp);
	sm_summary.out = dup(fileno(s->fp));    /* run_command closes it */
	run_command(&sm_summary);
}

static void wt_status_print_untracked(struct wt_status *s)
{
	int i;
	struct strbuf buf = STRBUF_INIT;

	if (!s->untracked.nr)
		return;

	wt_status_print_untracked_header(s);
	for (i = 0; i < s->untracked.nr; i++) {
		struct string_list_item *it;
		it = &(s->untracked.items[i]);
		color_fprintf(s->fp, color(WT_STATUS_HEADER, s), "#\t");
		color_fprintf_ln(s->fp, color(WT_STATUS_UNTRACKED, s), "%s",
				 quote_path(it->string, strlen(it->string),
					    &buf, s->prefix));
	}
	strbuf_release(&buf);
}

static void wt_status_print_verbose(struct wt_status *s)
{
	struct rev_info rev;

	init_revisions(&rev, NULL);
	DIFF_OPT_SET(&rev.diffopt, ALLOW_TEXTCONV);
	setup_revisions(0, NULL, &rev,
		s->is_initial ? EMPTY_TREE_SHA1_HEX : s->reference);
	rev.diffopt.output_format |= DIFF_FORMAT_PATCH;
	rev.diffopt.detect_rename = 1;
	rev.diffopt.file = s->fp;
	rev.diffopt.close_file = 0;
	/*
	 * If we're not going to stdout, then we definitely don't
	 * want color, since we are going to the commit message
	 * file (and even the "auto" setting won't work, since it
	 * will have checked isatty on stdout).
	 */
	if (s->fp != stdout)
		DIFF_OPT_CLR(&rev.diffopt, COLOR_DIFF);
	run_diff_index(&rev, 1);
}

static void wt_status_print_tracking(struct wt_status *s)
{
	struct strbuf sb = STRBUF_INIT;
	const char *cp, *ep;
	struct branch *branch;

	assert(s->branch && !s->is_initial);
	if (prefixcmp(s->branch, "refs/heads/"))
		return;
	branch = branch_get(s->branch + 11);
	if (!format_tracking_info(branch, &sb))
		return;

	for (cp = sb.buf; (ep = strchr(cp, '\n')) != NULL; cp = ep + 1)
		color_fprintf_ln(s->fp, color(WT_STATUS_HEADER, s),
				 "# %.*s", (int)(ep - cp), cp);
	color_fprintf_ln(s->fp, color(WT_STATUS_HEADER, s), "#");
}

void wt_status_print(struct wt_status *s)
{
	const char *branch_color = color(WT_STATUS_HEADER, s);

	if (s->branch) {
		const char *on_what = "On branch ";
		const char *branch_name = s->branch;
		if (!prefixcmp(branch_name, "refs/heads/"))
			branch_name += 11;
		else if (!strcmp(branch_name, "HEAD")) {
			branch_name = "";
			branch_color = color(WT_STATUS_NOBRANCH, s);
			on_what = "Not currently on any branch.";
		}
		color_fprintf(s->fp, color(WT_STATUS_HEADER, s), "# ");
		color_fprintf_ln(s->fp, branch_color, "%s%s", on_what, branch_name);
		if (!s->is_initial)
			wt_status_print_tracking(s);
	}

	if (s->is_initial) {
		color_fprintf_ln(s->fp, color(WT_STATUS_HEADER, s), "#");
		color_fprintf_ln(s->fp, color(WT_STATUS_HEADER, s), "# Initial commit");
		color_fprintf_ln(s->fp, color(WT_STATUS_HEADER, s), "#");
	}

	wt_status_print_updated(s);
	wt_status_print_unmerged(s);
	wt_status_print_changed(s);
	if (s->submodule_summary)
		wt_status_print_submodule_summary(s);
	if (s->show_untracked_files)
		wt_status_print_untracked(s);
	else if (s->commitable)
		 fprintf(s->fp, "# Untracked files not listed (use -u option to show untracked files)\n");

	if (s->verbose)
		wt_status_print_verbose(s);
	if (!s->commitable) {
		if (s->amend)
			fprintf(s->fp, "# No changes\n");
		else if (s->nowarn)
			; /* nothing */
		else if (s->workdir_dirty)
			printf("no changes added to commit (use \"git add\" and/or \"git commit -a\")\n");
		else if (s->untracked.nr)
			printf("nothing added to commit but untracked files present (use \"git add\" to track)\n");
		else if (s->is_initial)
			printf("nothing to commit (create/copy files and use \"git add\" to track)\n");
		else if (!s->show_untracked_files)
			printf("nothing to commit (use -u to show untracked files)\n");
		else
			printf("nothing to commit (working directory clean)\n");
	}
}

static void wt_shortstatus_unmerged(int null_termination, struct string_list_item *it,
			   struct wt_status *s)
{
	struct wt_status_change_data *d = it->util;
	const char *how = "??";

	switch (d->stagemask) {
	case 1: how = "DD"; break; /* both deleted */
	case 2: how = "AU"; break; /* added by us */
	case 3: how = "UD"; break; /* deleted by them */
	case 4: how = "UA"; break; /* added by them */
	case 5: how = "DU"; break; /* deleted by us */
	case 6: how = "AA"; break; /* both added */
	case 7: how = "UU"; break; /* both modified */
	}
	color_fprintf(s->fp, color(WT_STATUS_UNMERGED, s), "%s", how);
	if (null_termination) {
		fprintf(stdout, " %s%c", it->string, 0);
	} else {
		struct strbuf onebuf = STRBUF_INIT;
		const char *one;
		one = quote_path(it->string, -1, &onebuf, s->prefix);
		printf(" %s\n", one);
		strbuf_release(&onebuf);
	}
}

static void wt_shortstatus_status(int null_termination, struct string_list_item *it,
			 struct wt_status *s)
{
	struct wt_status_change_data *d = it->util;

	if (d->index_status)
		color_fprintf(s->fp, color(WT_STATUS_UPDATED, s), "%c", d->index_status);
	else
		putchar(' ');
	if (d->worktree_status)
		color_fprintf(s->fp, color(WT_STATUS_CHANGED, s), "%c", d->worktree_status);
	else
		putchar(' ');
	putchar(' ');
	if (null_termination) {
		fprintf(stdout, "%s%c", it->string, 0);
		if (d->head_path)
			fprintf(stdout, "%s%c", d->head_path, 0);
	} else {
		struct strbuf onebuf = STRBUF_INIT;
		const char *one;
		if (d->head_path) {
			one = quote_path(d->head_path, -1, &onebuf, s->prefix);
			printf("%s -> ", one);
			strbuf_release(&onebuf);
		}
		one = quote_path(it->string, -1, &onebuf, s->prefix);
		printf("%s\n", one);
		strbuf_release(&onebuf);
	}
}

static void wt_shortstatus_untracked(int null_termination, struct string_list_item *it,
			    struct wt_status *s)
{
	if (null_termination) {
		fprintf(stdout, "?? %s%c", it->string, 0);
	} else {
		struct strbuf onebuf = STRBUF_INIT;
		const char *one;
		one = quote_path(it->string, -1, &onebuf, s->prefix);
		color_fprintf(s->fp, color(WT_STATUS_UNTRACKED, s), "??");
		printf(" %s\n", one);
		strbuf_release(&onebuf);
	}
}

void wt_shortstatus_print(struct wt_status *s, int null_termination)
{
	int i;
	for (i = 0; i < s->change.nr; i++) {
		struct wt_status_change_data *d;
		struct string_list_item *it;

		it = &(s->change.items[i]);
		d = it->util;
		if (d->stagemask)
			wt_shortstatus_unmerged(null_termination, it, s);
		else
			wt_shortstatus_status(null_termination, it, s);
	}
	for (i = 0; i < s->untracked.nr; i++) {
		struct string_list_item *it;

		it = &(s->untracked.items[i]);
		wt_shortstatus_untracked(null_termination, it, s);
	}
}

void wt_porcelain_print(struct wt_status *s, int null_termination)
{
	s->use_color = 0;
	s->relative_paths = 0;
	s->prefix = NULL;
	wt_shortstatus_print(s, null_termination);
}
