/*
 * Builtin "git commit"
 *
 * Copyright (c) 2007 Kristian Høgsberg <krh@redhat.com>
 * Based on git-commit.sh by Junio C Hamano and Linus Torvalds
 */

#include "cache.h"
#include "cache-tree.h"
#include "color.h"
#include "dir.h"
#include "builtin.h"
#include "diff.h"
#include "diffcore.h"
#include "commit.h"
#include "revision.h"
#include "wt-status.h"
#include "run-command.h"
#include "refs.h"
#include "log-tree.h"
#include "strbuf.h"
#include "utf8.h"
#include "parse-options.h"
#include "string-list.h"
#include "rerere.h"
#include "unpack-trees.h"
#include "quote.h"
#include "submodule.h"
#include "gpg-interface.h"
#include "column.h"
#include "sequencer.h"
#include "notes-utils.h"

static const char * const builtin_commit_usage[] = {
	N_("git commit [options] [--] <pathspec>..."),
	NULL
};

static const char * const builtin_status_usage[] = {
	N_("git status [options] [--] <pathspec>..."),
	NULL
};

static const char implicit_ident_advice[] =
N_("Your name and email address were configured automatically based\n"
"on your username and hostname. Please check that they are accurate.\n"
"You can suppress this message by setting them explicitly:\n"
"\n"
"    git config --global user.name \"Your Name\"\n"
"    git config --global user.email you@example.com\n"
"\n"
"After doing this, you may fix the identity used for this commit with:\n"
"\n"
"    git commit --amend --reset-author\n");

static const char empty_amend_advice[] =
N_("You asked to amend the most recent commit, but doing so would make\n"
"it empty. You can repeat your command with --allow-empty, or you can\n"
"remove the commit entirely with \"git reset HEAD^\".\n");

static const char empty_cherry_pick_advice[] =
N_("The previous cherry-pick is now empty, possibly due to conflict resolution.\n"
"If you wish to commit it anyway, use:\n"
"\n"
"    git commit --allow-empty\n"
"\n"
"Otherwise, please use 'git reset'\n");

static const char *use_message_buffer;
static const char commit_editmsg[] = "COMMIT_EDITMSG";
static struct lock_file index_lock; /* real index */
static struct lock_file false_lock; /* used only for partial commits */
static enum {
	COMMIT_AS_IS = 1,
	COMMIT_NORMAL,
	COMMIT_PARTIAL
} commit_style;

static const char *logfile, *force_author;
static const char *template_file;
/*
 * The _message variables are commit names from which to take
 * the commit message and/or authorship.
 */
static const char *author_message, *author_message_buffer;
static char *edit_message, *use_message;
static char *fixup_message, *squash_message;
static int all, also, interactive, patch_interactive, only, amend, signoff;
static int edit_flag = -1; /* unspecified */
static int quiet, verbose, no_verify, allow_empty, dry_run, renew_authorship;
static int no_post_rewrite, allow_empty_message;
static char *untracked_files_arg, *force_date, *ignore_submodule_arg;
static char *sign_commit;

/*
 * The default commit message cleanup mode will remove the lines
 * beginning with # (shell comments) and leading and trailing
 * whitespaces (empty lines or containing only whitespaces)
 * if editor is used, and only the whitespaces if the message
 * is specified explicitly.
 */
static enum {
	CLEANUP_SPACE,
	CLEANUP_NONE,
	CLEANUP_ALL
} cleanup_mode;
static const char *cleanup_arg;

static enum commit_whence whence;
static int use_editor = 1, include_status = 1;
static int show_ignored_in_status, have_option_m;
static const char *only_include_assumed;
static struct strbuf message = STRBUF_INIT;

static enum status_format {
	STATUS_FORMAT_NONE = 0,
	STATUS_FORMAT_LONG,
	STATUS_FORMAT_SHORT,
	STATUS_FORMAT_PORCELAIN,

	STATUS_FORMAT_UNSPECIFIED
} status_format = STATUS_FORMAT_UNSPECIFIED;

static int opt_parse_m(const struct option *opt, const char *arg, int unset)
{
	struct strbuf *buf = opt->value;
	if (unset) {
		have_option_m = 0;
		strbuf_setlen(buf, 0);
	} else {
		have_option_m = 1;
		if (buf->len)
			strbuf_addch(buf, '\n');
		strbuf_addstr(buf, arg);
		strbuf_complete_line(buf);
	}
	return 0;
}

static void determine_whence(struct wt_status *s)
{
	if (file_exists(git_path("MERGE_HEAD")))
		whence = FROM_MERGE;
	else if (file_exists(git_path("CHERRY_PICK_HEAD")))
		whence = FROM_CHERRY_PICK;
	else
		whence = FROM_COMMIT;
	if (s)
		s->whence = whence;
}

static void rollback_index_files(void)
{
	switch (commit_style) {
	case COMMIT_AS_IS:
		break; /* nothing to do */
	case COMMIT_NORMAL:
		rollback_lock_file(&index_lock);
		break;
	case COMMIT_PARTIAL:
		rollback_lock_file(&index_lock);
		rollback_lock_file(&false_lock);
		break;
	}
}

static int commit_index_files(void)
{
	int err = 0;

	switch (commit_style) {
	case COMMIT_AS_IS:
		break; /* nothing to do */
	case COMMIT_NORMAL:
		err = commit_lock_file(&index_lock);
		break;
	case COMMIT_PARTIAL:
		err = commit_lock_file(&index_lock);
		rollback_lock_file(&false_lock);
		break;
	}

	return err;
}

/*
 * Take a union of paths in the index and the named tree (typically, "HEAD"),
 * and return the paths that match the given pattern in list.
 */
static int list_paths(struct string_list *list, const char *with_tree,
		      const char *prefix, const struct pathspec *pattern)
{
	int i;
	char *m;

	if (!pattern->nr)
		return 0;

	m = xcalloc(1, pattern->nr);

	if (with_tree) {
		char *max_prefix = common_prefix(pattern);
		overlay_tree_on_cache(with_tree, max_prefix ? max_prefix : prefix);
		free(max_prefix);
	}

	for (i = 0; i < active_nr; i++) {
		const struct cache_entry *ce = active_cache[i];
		struct string_list_item *item;

		if (ce->ce_flags & CE_UPDATE)
			continue;
		if (!match_pathspec_depth(pattern, ce->name, ce_namelen(ce), 0, m))
			continue;
		item = string_list_insert(list, ce->name);
		if (ce_skip_worktree(ce))
			item->util = item; /* better a valid pointer than a fake one */
	}

	return report_path_error(m, pattern, prefix);
}

static void add_remove_files(struct string_list *list)
{
	int i;
	for (i = 0; i < list->nr; i++) {
		struct stat st;
		struct string_list_item *p = &(list->items[i]);

		/* p->util is skip-worktree */
		if (p->util)
			continue;

		if (!lstat(p->string, &st)) {
			if (add_to_cache(p->string, &st, 0))
				die(_("updating files failed"));
		} else
			remove_file_from_cache(p->string);
	}
}

static void create_base_index(const struct commit *current_head)
{
	struct tree *tree;
	struct unpack_trees_options opts;
	struct tree_desc t;

	if (!current_head) {
		discard_cache();
		return;
	}

	memset(&opts, 0, sizeof(opts));
	opts.head_idx = 1;
	opts.index_only = 1;
	opts.merge = 1;
	opts.src_index = &the_index;
	opts.dst_index = &the_index;

	opts.fn = oneway_merge;
	tree = parse_tree_indirect(current_head->object.sha1);
	if (!tree)
		die(_("failed to unpack HEAD tree object"));
	parse_tree(tree);
	init_tree_desc(&t, tree->buffer, tree->size);
	if (unpack_trees(1, &t, &opts))
		exit(128); /* We've already reported the error, finish dying */
}

static void refresh_cache_or_die(int refresh_flags)
{
	/*
	 * refresh_flags contains REFRESH_QUIET, so the only errors
	 * are for unmerged entries.
	 */
	if (refresh_cache(refresh_flags | REFRESH_IN_PORCELAIN))
		die_resolve_conflict("commit");
}

static char *prepare_index(int argc, const char **argv, const char *prefix,
			   const struct commit *current_head, int is_status)
{
	int fd;
	struct string_list partial;
	struct pathspec pathspec;
	char *old_index_env = NULL;
	int refresh_flags = REFRESH_QUIET;

	if (is_status)
		refresh_flags |= REFRESH_UNMERGED;
	parse_pathspec(&pathspec, 0,
		       PATHSPEC_PREFER_FULL,
		       prefix, argv);

	if (read_cache_preload(&pathspec) < 0)
		die(_("index file corrupt"));

	if (interactive) {
		fd = hold_locked_index(&index_lock, 1);

		refresh_cache_or_die(refresh_flags);

		if (write_cache(fd, active_cache, active_nr) ||
		    close_lock_file(&index_lock))
			die(_("unable to create temporary index"));

		old_index_env = getenv(INDEX_ENVIRONMENT);
		setenv(INDEX_ENVIRONMENT, index_lock.filename, 1);

		if (interactive_add(argc, argv, prefix, patch_interactive) != 0)
			die(_("interactive add failed"));

		if (old_index_env && *old_index_env)
			setenv(INDEX_ENVIRONMENT, old_index_env, 1);
		else
			unsetenv(INDEX_ENVIRONMENT);

		discard_cache();
		read_cache_from(index_lock.filename);

		commit_style = COMMIT_NORMAL;
		return index_lock.filename;
	}

	/*
	 * Non partial, non as-is commit.
	 *
	 * (1) get the real index;
	 * (2) update the_index as necessary;
	 * (3) write the_index out to the real index (still locked);
	 * (4) return the name of the locked index file.
	 *
	 * The caller should run hooks on the locked real index, and
	 * (A) if all goes well, commit the real index;
	 * (B) on failure, rollback the real index.
	 */
	if (all || (also && pathspec.nr)) {
		fd = hold_locked_index(&index_lock, 1);
		add_files_to_cache(also ? prefix : NULL, &pathspec, 0);
		refresh_cache_or_die(refresh_flags);
		update_main_cache_tree(WRITE_TREE_SILENT);
		if (write_cache(fd, active_cache, active_nr) ||
		    close_lock_file(&index_lock))
			die(_("unable to write new_index file"));
		commit_style = COMMIT_NORMAL;
		return index_lock.filename;
	}

	/*
	 * As-is commit.
	 *
	 * (1) return the name of the real index file.
	 *
	 * The caller should run hooks on the real index,
	 * and create commit from the_index.
	 * We still need to refresh the index here.
	 */
	if (!only && !pathspec.nr) {
		fd = hold_locked_index(&index_lock, 1);
		refresh_cache_or_die(refresh_flags);
		if (active_cache_changed) {
			update_main_cache_tree(WRITE_TREE_SILENT);
			if (write_cache(fd, active_cache, active_nr) ||
			    commit_locked_index(&index_lock))
				die(_("unable to write new_index file"));
		} else {
			rollback_lock_file(&index_lock);
		}
		commit_style = COMMIT_AS_IS;
		return get_index_file();
	}

	/*
	 * A partial commit.
	 *
	 * (0) find the set of affected paths;
	 * (1) get lock on the real index file;
	 * (2) update the_index with the given paths;
	 * (3) write the_index out to the real index (still locked);
	 * (4) get lock on the false index file;
	 * (5) reset the_index from HEAD;
	 * (6) update the_index the same way as (2);
	 * (7) write the_index out to the false index file;
	 * (8) return the name of the false index file (still locked);
	 *
	 * The caller should run hooks on the locked false index, and
	 * create commit from it.  Then
	 * (A) if all goes well, commit the real index;
	 * (B) on failure, rollback the real index;
	 * In either case, rollback the false index.
	 */
	commit_style = COMMIT_PARTIAL;

	if (whence != FROM_COMMIT) {
		if (whence == FROM_MERGE)
			die(_("cannot do a partial commit during a merge."));
		else if (whence == FROM_CHERRY_PICK)
			die(_("cannot do a partial commit during a cherry-pick."));
	}

	memset(&partial, 0, sizeof(partial));
	partial.strdup_strings = 1;
	if (list_paths(&partial, !current_head ? NULL : "HEAD", prefix, &pathspec))
		exit(1);

	discard_cache();
	if (read_cache() < 0)
		die(_("cannot read the index"));

	fd = hold_locked_index(&index_lock, 1);
	add_remove_files(&partial);
	refresh_cache(REFRESH_QUIET);
	if (write_cache(fd, active_cache, active_nr) ||
	    close_lock_file(&index_lock))
		die(_("unable to write new_index file"));

	fd = hold_lock_file_for_update(&false_lock,
				       git_path("next-index-%"PRIuMAX,
						(uintmax_t) getpid()),
				       LOCK_DIE_ON_ERROR);

	create_base_index(current_head);
	add_remove_files(&partial);
	refresh_cache(REFRESH_QUIET);

	if (write_cache(fd, active_cache, active_nr) ||
	    close_lock_file(&false_lock))
		die(_("unable to write temporary index file"));

	discard_cache();
	read_cache_from(false_lock.filename);

	return false_lock.filename;
}

static int run_status(FILE *fp, const char *index_file, const char *prefix, int nowarn,
		      struct wt_status *s)
{
	unsigned char sha1[20];

	if (s->relative_paths)
		s->prefix = prefix;

	if (amend) {
		s->amend = 1;
		s->reference = "HEAD^1";
	}
	s->verbose = verbose;
	s->index_file = index_file;
	s->fp = fp;
	s->nowarn = nowarn;
	s->is_initial = get_sha1(s->reference, sha1) ? 1 : 0;

	wt_status_collect(s);

	switch (status_format) {
	case STATUS_FORMAT_SHORT:
		wt_shortstatus_print(s);
		break;
	case STATUS_FORMAT_PORCELAIN:
		wt_porcelain_print(s);
		break;
	case STATUS_FORMAT_UNSPECIFIED:
		die("BUG: finalize_deferred_config() should have been called");
		break;
	case STATUS_FORMAT_NONE:
	case STATUS_FORMAT_LONG:
		wt_status_print(s);
		break;
	}

	return s->commitable;
}

static int is_a_merge(const struct commit *current_head)
{
	return !!(current_head->parents && current_head->parents->next);
}

static void export_one(const char *var, const char *s, const char *e, int hack)
{
	struct strbuf buf = STRBUF_INIT;
	if (hack)
		strbuf_addch(&buf, hack);
	strbuf_addf(&buf, "%.*s", (int)(e - s), s);
	setenv(var, buf.buf, 1);
	strbuf_release(&buf);
}

static int sane_ident_split(struct ident_split *person)
{
	if (!person->name_begin || !person->name_end ||
	    person->name_begin == person->name_end)
		return 0; /* no human readable name */
	if (!person->mail_begin || !person->mail_end ||
	    person->mail_begin == person->mail_end)
		return 0; /* no usable mail */
	if (!person->date_begin || !person->date_end ||
	    !person->tz_begin || !person->tz_end)
		return 0;
	return 1;
}

static void determine_author_info(struct strbuf *author_ident)
{
	char *name, *email, *date;
	struct ident_split author;

	name = getenv("GIT_AUTHOR_NAME");
	email = getenv("GIT_AUTHOR_EMAIL");
	date = getenv("GIT_AUTHOR_DATE");

	if (author_message) {
		const char *a, *lb, *rb, *eol;
		size_t len;

		a = strstr(author_message_buffer, "\nauthor ");
		if (!a)
			die(_("invalid commit: %s"), author_message);

		lb = strchrnul(a + strlen("\nauthor "), '<');
		rb = strchrnul(lb, '>');
		eol = strchrnul(rb, '\n');
		if (!*lb || !*rb || !*eol)
			die(_("invalid commit: %s"), author_message);

		if (lb == a + strlen("\nauthor "))
			/* \nauthor <foo@example.com> */
			name = xcalloc(1, 1);
		else
			name = xmemdupz(a + strlen("\nauthor "),
					(lb - strlen(" ") -
					 (a + strlen("\nauthor "))));
		email = xmemdupz(lb + strlen("<"), rb - (lb + strlen("<")));
		len = eol - (rb + strlen("> "));
		date = xmalloc(len + 2);
		*date = '@';
		memcpy(date + 1, rb + strlen("> "), len);
		date[len + 1] = '\0';
	}

	if (force_author) {
		const char *lb = strstr(force_author, " <");
		const char *rb = strchr(force_author, '>');

		if (!lb || !rb)
			die(_("malformed --author parameter"));
		name = xstrndup(force_author, lb - force_author);
		email = xstrndup(lb + 2, rb - (lb + 2));
	}

	if (force_date)
		date = force_date;
	strbuf_addstr(author_ident, fmt_ident(name, email, date, IDENT_STRICT));
	if (!split_ident_line(&author, author_ident->buf, author_ident->len) &&
	    sane_ident_split(&author)) {
		export_one("GIT_AUTHOR_NAME", author.name_begin, author.name_end, 0);
		export_one("GIT_AUTHOR_EMAIL", author.mail_begin, author.mail_end, 0);
		export_one("GIT_AUTHOR_DATE", author.date_begin, author.tz_end, '@');
	}
}

static char *cut_ident_timestamp_part(char *string)
{
	char *ket = strrchr(string, '>');
	if (!ket || ket[1] != ' ')
		die(_("Malformed ident string: '%s'"), string);
	*++ket = '\0';
	return ket;
}

static int prepare_to_commit(const char *index_file, const char *prefix,
			     struct commit *current_head,
			     struct wt_status *s,
			     struct strbuf *author_ident)
{
	struct stat statbuf;
	struct strbuf committer_ident = STRBUF_INIT;
	int commitable, saved_color_setting;
	struct strbuf sb = STRBUF_INIT;
	char *buffer;
	const char *hook_arg1 = NULL;
	const char *hook_arg2 = NULL;
	int ident_shown = 0;
	int clean_message_contents = (cleanup_mode != CLEANUP_NONE);

	/* This checks and barfs if author is badly specified */
	determine_author_info(author_ident);

	if (!no_verify && run_hook(index_file, "pre-commit", NULL))
		return 0;

	if (squash_message) {
		/*
		 * Insert the proper subject line before other commit
		 * message options add their content.
		 */
		if (use_message && !strcmp(use_message, squash_message))
			strbuf_addstr(&sb, "squash! ");
		else {
			struct pretty_print_context ctx = {0};
			struct commit *c;
			c = lookup_commit_reference_by_name(squash_message);
			if (!c)
				die(_("could not lookup commit %s"), squash_message);
			ctx.output_encoding = get_commit_output_encoding();
			format_commit_message(c, "squash! %s\n\n", &sb,
					      &ctx);
		}
	}

	if (message.len) {
		strbuf_addbuf(&sb, &message);
		hook_arg1 = "message";
	} else if (logfile && !strcmp(logfile, "-")) {
		if (isatty(0))
			fprintf(stderr, _("(reading log message from standard input)\n"));
		if (strbuf_read(&sb, 0, 0) < 0)
			die_errno(_("could not read log from standard input"));
		hook_arg1 = "message";
	} else if (logfile) {
		if (strbuf_read_file(&sb, logfile, 0) < 0)
			die_errno(_("could not read log file '%s'"),
				  logfile);
		hook_arg1 = "message";
	} else if (use_message) {
		buffer = strstr(use_message_buffer, "\n\n");
		if (!use_editor && (!buffer || buffer[2] == '\0'))
			die(_("commit has empty message"));
		strbuf_add(&sb, buffer + 2, strlen(buffer + 2));
		hook_arg1 = "commit";
		hook_arg2 = use_message;
	} else if (fixup_message) {
		struct pretty_print_context ctx = {0};
		struct commit *commit;
		commit = lookup_commit_reference_by_name(fixup_message);
		if (!commit)
			die(_("could not lookup commit %s"), fixup_message);
		ctx.output_encoding = get_commit_output_encoding();
		format_commit_message(commit, "fixup! %s\n\n",
				      &sb, &ctx);
		hook_arg1 = "message";
	} else if (!stat(git_path("MERGE_MSG"), &statbuf)) {
		if (strbuf_read_file(&sb, git_path("MERGE_MSG"), 0) < 0)
			die_errno(_("could not read MERGE_MSG"));
		hook_arg1 = "merge";
	} else if (!stat(git_path("SQUASH_MSG"), &statbuf)) {
		if (strbuf_read_file(&sb, git_path("SQUASH_MSG"), 0) < 0)
			die_errno(_("could not read SQUASH_MSG"));
		hook_arg1 = "squash";
	} else if (template_file) {
		if (strbuf_read_file(&sb, template_file, 0) < 0)
			die_errno(_("could not read '%s'"), template_file);
		hook_arg1 = "template";
		clean_message_contents = 0;
	}

	/*
	 * The remaining cases don't modify the template message, but
	 * just set the argument(s) to the prepare-commit-msg hook.
	 */
	else if (whence == FROM_MERGE)
		hook_arg1 = "merge";
	else if (whence == FROM_CHERRY_PICK) {
		hook_arg1 = "commit";
		hook_arg2 = "CHERRY_PICK_HEAD";
	}

	if (squash_message) {
		/*
		 * If squash_commit was used for the commit subject,
		 * then we're possibly hijacking other commit log options.
		 * Reset the hook args to tell the real story.
		 */
		hook_arg1 = "message";
		hook_arg2 = "";
	}

	s->fp = fopen(git_path(commit_editmsg), "w");
	if (s->fp == NULL)
		die_errno(_("could not open '%s'"), git_path(commit_editmsg));

	if (clean_message_contents)
		stripspace(&sb, 0);

	if (signoff) {
		/*
		 * See if we have a Conflicts: block at the end. If yes, count
		 * its size, so we can ignore it.
		 */
		int ignore_footer = 0;
		int i, eol, previous = 0;
		const char *nl;

		for (i = 0; i < sb.len; i++) {
			nl = memchr(sb.buf + i, '\n', sb.len - i);
			if (nl)
				eol = nl - sb.buf;
			else
				eol = sb.len;
			if (!prefixcmp(sb.buf + previous, "\nConflicts:\n")) {
				ignore_footer = sb.len - previous;
				break;
			}
			while (i < eol)
				i++;
			previous = eol;
		}

		append_signoff(&sb, ignore_footer, 0);
	}

	if (fwrite(sb.buf, 1, sb.len, s->fp) < sb.len)
		die_errno(_("could not write commit template"));

	strbuf_release(&sb);

	/* This checks if committer ident is explicitly given */
	strbuf_addstr(&committer_ident, git_committer_info(IDENT_STRICT));
	if (use_editor && include_status) {
		char *ai_tmp, *ci_tmp;
		if (whence != FROM_COMMIT)
			status_printf_ln(s, GIT_COLOR_NORMAL,
			    whence == FROM_MERGE
				? _("\n"
					"It looks like you may be committing a merge.\n"
					"If this is not correct, please remove the file\n"
					"	%s\n"
					"and try again.\n")
				: _("\n"
					"It looks like you may be committing a cherry-pick.\n"
					"If this is not correct, please remove the file\n"
					"	%s\n"
					"and try again.\n"),
				git_path(whence == FROM_MERGE
					 ? "MERGE_HEAD"
					 : "CHERRY_PICK_HEAD"));

		fprintf(s->fp, "\n");
		if (cleanup_mode == CLEANUP_ALL)
			status_printf(s, GIT_COLOR_NORMAL,
				_("Please enter the commit message for your changes."
				  " Lines starting\nwith '%c' will be ignored, and an empty"
				  " message aborts the commit.\n"), comment_line_char);
		else /* CLEANUP_SPACE, that is. */
			status_printf(s, GIT_COLOR_NORMAL,
				_("Please enter the commit message for your changes."
				  " Lines starting\n"
				  "with '%c' will be kept; you may remove them"
				  " yourself if you want to.\n"
				  "An empty message aborts the commit.\n"), comment_line_char);
		if (only_include_assumed)
			status_printf_ln(s, GIT_COLOR_NORMAL,
					"%s", only_include_assumed);

		ai_tmp = cut_ident_timestamp_part(author_ident->buf);
		ci_tmp = cut_ident_timestamp_part(committer_ident.buf);
		if (strcmp(author_ident->buf, committer_ident.buf))
			status_printf_ln(s, GIT_COLOR_NORMAL,
				_("%s"
				"Author:    %s"),
				ident_shown++ ? "" : "\n",
				author_ident->buf);

		if (!committer_ident_sufficiently_given())
			status_printf_ln(s, GIT_COLOR_NORMAL,
				_("%s"
				"Committer: %s"),
				ident_shown++ ? "" : "\n",
				committer_ident.buf);

		if (ident_shown)
			status_printf_ln(s, GIT_COLOR_NORMAL, "");

		saved_color_setting = s->use_color;
		s->use_color = 0;
		commitable = run_status(s->fp, index_file, prefix, 1, s);
		s->use_color = saved_color_setting;

		*ai_tmp = ' ';
		*ci_tmp = ' ';
	} else {
		unsigned char sha1[20];
		const char *parent = "HEAD";

		if (!active_nr && read_cache() < 0)
			die(_("Cannot read index"));

		if (amend)
			parent = "HEAD^1";

		if (get_sha1(parent, sha1))
			commitable = !!active_nr;
		else
			commitable = index_differs_from(parent, 0);
	}
	strbuf_release(&committer_ident);

	fclose(s->fp);

	/*
	 * Reject an attempt to record a non-merge empty commit without
	 * explicit --allow-empty. In the cherry-pick case, it may be
	 * empty due to conflict resolution, which the user should okay.
	 */
	if (!commitable && whence != FROM_MERGE && !allow_empty &&
	    !(amend && is_a_merge(current_head))) {
		run_status(stdout, index_file, prefix, 0, s);
		if (amend)
			fputs(_(empty_amend_advice), stderr);
		else if (whence == FROM_CHERRY_PICK)
			fputs(_(empty_cherry_pick_advice), stderr);
		return 0;
	}

	/*
	 * Re-read the index as pre-commit hook could have updated it,
	 * and write it out as a tree.  We must do this before we invoke
	 * the editor and after we invoke run_status above.
	 */
	discard_cache();
	read_cache_from(index_file);
	if (update_main_cache_tree(0)) {
		error(_("Error building trees"));
		return 0;
	}

	if (run_hook(index_file, "prepare-commit-msg",
		     git_path(commit_editmsg), hook_arg1, hook_arg2, NULL))
		return 0;

	if (use_editor) {
		char index[PATH_MAX];
		const char *env[2] = { NULL };
		env[0] =  index;
		snprintf(index, sizeof(index), "GIT_INDEX_FILE=%s", index_file);
		if (launch_editor(git_path(commit_editmsg), NULL, env)) {
			fprintf(stderr,
			_("Please supply the message using either -m or -F option.\n"));
			exit(1);
		}
	}

	if (!no_verify &&
	    run_hook(index_file, "commit-msg", git_path(commit_editmsg), NULL)) {
		return 0;
	}

	return 1;
}

static int rest_is_empty(struct strbuf *sb, int start)
{
	int i, eol;
	const char *nl;

	/* Check if the rest is just whitespace and Signed-of-by's. */
	for (i = start; i < sb->len; i++) {
		nl = memchr(sb->buf + i, '\n', sb->len - i);
		if (nl)
			eol = nl - sb->buf;
		else
			eol = sb->len;

		if (strlen(sign_off_header) <= eol - i &&
		    !prefixcmp(sb->buf + i, sign_off_header)) {
			i = eol;
			continue;
		}
		while (i < eol)
			if (!isspace(sb->buf[i++]))
				return 0;
	}

	return 1;
}

/*
 * Find out if the message in the strbuf contains only whitespace and
 * Signed-off-by lines.
 */
static int message_is_empty(struct strbuf *sb)
{
	if (cleanup_mode == CLEANUP_NONE && sb->len)
		return 0;
	return rest_is_empty(sb, 0);
}

/*
 * See if the user edited the message in the editor or left what
 * was in the template intact
 */
static int template_untouched(struct strbuf *sb)
{
	struct strbuf tmpl = STRBUF_INIT;
	char *start;

	if (cleanup_mode == CLEANUP_NONE && sb->len)
		return 0;

	if (!template_file || strbuf_read_file(&tmpl, template_file, 0) <= 0)
		return 0;

	stripspace(&tmpl, cleanup_mode == CLEANUP_ALL);
	start = (char *)skip_prefix(sb->buf, tmpl.buf);
	if (!start)
		start = sb->buf;
	strbuf_release(&tmpl);
	return rest_is_empty(sb, start - sb->buf);
}

static const char *find_author_by_nickname(const char *name)
{
	struct rev_info revs;
	struct commit *commit;
	struct strbuf buf = STRBUF_INIT;
	const char *av[20];
	int ac = 0;

	init_revisions(&revs, NULL);
	strbuf_addf(&buf, "--author=%s", name);
	av[++ac] = "--all";
	av[++ac] = "-i";
	av[++ac] = buf.buf;
	av[++ac] = NULL;
	setup_revisions(ac, av, &revs, NULL);
	prepare_revision_walk(&revs);
	commit = get_revision(&revs);
	if (commit) {
		struct pretty_print_context ctx = {0};
		ctx.date_mode = DATE_NORMAL;
		strbuf_release(&buf);
		format_commit_message(commit, "%an <%ae>", &buf, &ctx);
		return strbuf_detach(&buf, NULL);
	}
	die(_("No existing author found with '%s'"), name);
}


static void handle_untracked_files_arg(struct wt_status *s)
{
	if (!untracked_files_arg)
		; /* default already initialized */
	else if (!strcmp(untracked_files_arg, "no"))
		s->show_untracked_files = SHOW_NO_UNTRACKED_FILES;
	else if (!strcmp(untracked_files_arg, "normal"))
		s->show_untracked_files = SHOW_NORMAL_UNTRACKED_FILES;
	else if (!strcmp(untracked_files_arg, "all"))
		s->show_untracked_files = SHOW_ALL_UNTRACKED_FILES;
	else
		die(_("Invalid untracked files mode '%s'"), untracked_files_arg);
}

static const char *read_commit_message(const char *name)
{
	const char *out_enc;
	struct commit *commit;

	commit = lookup_commit_reference_by_name(name);
	if (!commit)
		die(_("could not lookup commit %s"), name);
	out_enc = get_commit_output_encoding();
	return logmsg_reencode(commit, NULL, out_enc);
}

/*
 * Enumerate what needs to be propagated when --porcelain
 * is not in effect here.
 */
static struct status_deferred_config {
	enum status_format status_format;
	int show_branch;
} status_deferred_config = {
	STATUS_FORMAT_UNSPECIFIED,
	-1 /* unspecified */
};

static void finalize_deferred_config(struct wt_status *s)
{
	int use_deferred_config = (status_format != STATUS_FORMAT_PORCELAIN &&
				   !s->null_termination);

	if (s->null_termination) {
		if (status_format == STATUS_FORMAT_NONE ||
		    status_format == STATUS_FORMAT_UNSPECIFIED)
			status_format = STATUS_FORMAT_PORCELAIN;
		else if (status_format == STATUS_FORMAT_LONG)
			die(_("--long and -z are incompatible"));
	}

	if (use_deferred_config && status_format == STATUS_FORMAT_UNSPECIFIED)
		status_format = status_deferred_config.status_format;
	if (status_format == STATUS_FORMAT_UNSPECIFIED)
		status_format = STATUS_FORMAT_NONE;

	if (use_deferred_config && s->show_branch < 0)
		s->show_branch = status_deferred_config.show_branch;
	if (s->show_branch < 0)
		s->show_branch = 0;
}

static int parse_and_validate_options(int argc, const char *argv[],
				      const struct option *options,
				      const char * const usage[],
				      const char *prefix,
				      struct commit *current_head,
				      struct wt_status *s)
{
	int f = 0;

	argc = parse_options(argc, argv, prefix, options, usage, 0);
	finalize_deferred_config(s);

	if (force_author && !strchr(force_author, '>'))
		force_author = find_author_by_nickname(force_author);

	if (force_author && renew_authorship)
		die(_("Using both --reset-author and --author does not make sense"));

	if (logfile || have_option_m || use_message || fixup_message)
		use_editor = 0;
	if (0 <= edit_flag)
		use_editor = edit_flag;
	if (!use_editor)
		setenv("GIT_EDITOR", ":", 1);

	/* Sanity check options */
	if (amend && !current_head)
		die(_("You have nothing to amend."));
	if (amend && whence != FROM_COMMIT) {
		if (whence == FROM_MERGE)
			die(_("You are in the middle of a merge -- cannot amend."));
		else if (whence == FROM_CHERRY_PICK)
			die(_("You are in the middle of a cherry-pick -- cannot amend."));
	}
	if (fixup_message && squash_message)
		die(_("Options --squash and --fixup cannot be used together"));
	if (use_message)
		f++;
	if (edit_message)
		f++;
	if (fixup_message)
		f++;
	if (logfile)
		f++;
	if (f > 1)
		die(_("Only one of -c/-C/-F/--fixup can be used."));
	if (message.len && f > 0)
		die((_("Option -m cannot be combined with -c/-C/-F/--fixup.")));
	if (f || message.len)
		template_file = NULL;
	if (edit_message)
		use_message = edit_message;
	if (amend && !use_message && !fixup_message)
		use_message = "HEAD";
	if (!use_message && whence != FROM_CHERRY_PICK && renew_authorship)
		die(_("--reset-author can be used only with -C, -c or --amend."));
	if (use_message) {
		use_message_buffer = read_commit_message(use_message);
		if (!renew_authorship) {
			author_message = use_message;
			author_message_buffer = use_message_buffer;
		}
	}
	if (whence == FROM_CHERRY_PICK && !renew_authorship) {
		author_message = "CHERRY_PICK_HEAD";
		author_message_buffer = read_commit_message(author_message);
	}

	if (patch_interactive)
		interactive = 1;

	if (!!also + !!only + !!all + !!interactive > 1)
		die(_("Only one of --include/--only/--all/--interactive/--patch can be used."));
	if (argc == 0 && (also || (only && !amend)))
		die(_("No paths with --include/--only does not make sense."));
	if (argc == 0 && only && amend)
		only_include_assumed = _("Clever... amending the last one with dirty index.");
	if (argc > 0 && !also && !only)
		only_include_assumed = _("Explicit paths specified without -i nor -o; assuming --only paths...");
	if (!cleanup_arg || !strcmp(cleanup_arg, "default"))
		cleanup_mode = use_editor ? CLEANUP_ALL : CLEANUP_SPACE;
	else if (!strcmp(cleanup_arg, "verbatim"))
		cleanup_mode = CLEANUP_NONE;
	else if (!strcmp(cleanup_arg, "whitespace"))
		cleanup_mode = CLEANUP_SPACE;
	else if (!strcmp(cleanup_arg, "strip"))
		cleanup_mode = CLEANUP_ALL;
	else
		die(_("Invalid cleanup mode %s"), cleanup_arg);

	handle_untracked_files_arg(s);

	if (all && argc > 0)
		die(_("Paths with -a does not make sense."));

	if (status_format != STATUS_FORMAT_NONE)
		dry_run = 1;

	return argc;
}

static int dry_run_commit(int argc, const char **argv, const char *prefix,
			  const struct commit *current_head, struct wt_status *s)
{
	int commitable;
	const char *index_file;

	index_file = prepare_index(argc, argv, prefix, current_head, 1);
	commitable = run_status(stdout, index_file, prefix, 0, s);
	rollback_index_files();

	return commitable ? 0 : 1;
}

static int parse_status_slot(const char *var, int offset)
{
	if (!strcasecmp(var+offset, "header"))
		return WT_STATUS_HEADER;
	if (!strcasecmp(var+offset, "branch"))
		return WT_STATUS_ONBRANCH;
	if (!strcasecmp(var+offset, "updated")
		|| !strcasecmp(var+offset, "added"))
		return WT_STATUS_UPDATED;
	if (!strcasecmp(var+offset, "changed"))
		return WT_STATUS_CHANGED;
	if (!strcasecmp(var+offset, "untracked"))
		return WT_STATUS_UNTRACKED;
	if (!strcasecmp(var+offset, "nobranch"))
		return WT_STATUS_NOBRANCH;
	if (!strcasecmp(var+offset, "unmerged"))
		return WT_STATUS_UNMERGED;
	return -1;
}

static int git_status_config(const char *k, const char *v, void *cb)
{
	struct wt_status *s = cb;

	if (!prefixcmp(k, "column."))
		return git_column_config(k, v, "status", &s->colopts);
	if (!strcmp(k, "status.submodulesummary")) {
		int is_bool;
		s->submodule_summary = git_config_bool_or_int(k, v, &is_bool);
		if (is_bool && s->submodule_summary)
			s->submodule_summary = -1;
		return 0;
	}
	if (!strcmp(k, "status.short")) {
		if (git_config_bool(k, v))
			status_deferred_config.status_format = STATUS_FORMAT_SHORT;
		else
			status_deferred_config.status_format = STATUS_FORMAT_NONE;
		return 0;
	}
	if (!strcmp(k, "status.branch")) {
		status_deferred_config.show_branch = git_config_bool(k, v);
		return 0;
	}
	if (!strcmp(k, "status.color") || !strcmp(k, "color.status")) {
		s->use_color = git_config_colorbool(k, v);
		return 0;
	}
	if (!prefixcmp(k, "status.color.") || !prefixcmp(k, "color.status.")) {
		int slot = parse_status_slot(k, 13);
		if (slot < 0)
			return 0;
		if (!v)
			return config_error_nonbool(k);
		color_parse(v, k, s->color_palette[slot]);
		return 0;
	}
	if (!strcmp(k, "status.relativepaths")) {
		s->relative_paths = git_config_bool(k, v);
		return 0;
	}
	if (!strcmp(k, "status.showuntrackedfiles")) {
		if (!v)
			return config_error_nonbool(k);
		else if (!strcmp(v, "no"))
			s->show_untracked_files = SHOW_NO_UNTRACKED_FILES;
		else if (!strcmp(v, "normal"))
			s->show_untracked_files = SHOW_NORMAL_UNTRACKED_FILES;
		else if (!strcmp(v, "all"))
			s->show_untracked_files = SHOW_ALL_UNTRACKED_FILES;
		else
			return error(_("Invalid untracked files mode '%s'"), v);
		return 0;
	}
	return git_diff_ui_config(k, v, NULL);
}

int cmd_status(int argc, const char **argv, const char *prefix)
{
	static struct wt_status s;
	int fd;
	unsigned char sha1[20];
	static struct option builtin_status_options[] = {
		OPT__VERBOSE(&verbose, N_("be verbose")),
		OPT_SET_INT('s', "short", &status_format,
			    N_("show status concisely"), STATUS_FORMAT_SHORT),
		OPT_BOOL('b', "branch", &s.show_branch,
			 N_("show branch information")),
		OPT_SET_INT(0, "porcelain", &status_format,
			    N_("machine-readable output"),
			    STATUS_FORMAT_PORCELAIN),
		OPT_SET_INT(0, "long", &status_format,
			    N_("show status in long format (default)"),
			    STATUS_FORMAT_LONG),
		OPT_BOOLEAN('z', "null", &s.null_termination,
			    N_("terminate entries with NUL")),
		{ OPTION_STRING, 'u', "untracked-files", &untracked_files_arg,
		  N_("mode"),
		  N_("show untracked files, optional modes: all, normal, no. (Default: all)"),
		  PARSE_OPT_OPTARG, NULL, (intptr_t)"all" },
		OPT_BOOLEAN(0, "ignored", &show_ignored_in_status,
			    N_("show ignored files")),
		{ OPTION_STRING, 0, "ignore-submodules", &ignore_submodule_arg, N_("when"),
		  N_("ignore changes to submodules, optional when: all, dirty, untracked. (Default: all)"),
		  PARSE_OPT_OPTARG, NULL, (intptr_t)"all" },
		OPT_COLUMN(0, "column", &s.colopts, N_("list untracked files in columns")),
		OPT_END(),
	};

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(builtin_status_usage, builtin_status_options);

	wt_status_prepare(&s);
	gitmodules_config();
	git_config(git_status_config, &s);
	determine_whence(&s);
	argc = parse_options(argc, argv, prefix,
			     builtin_status_options,
			     builtin_status_usage, 0);
	finalize_colopts(&s.colopts, -1);
	finalize_deferred_config(&s);

	handle_untracked_files_arg(&s);
	if (show_ignored_in_status)
		s.show_ignored_files = 1;
	parse_pathspec(&s.pathspec, 0,
		       PATHSPEC_PREFER_FULL,
		       prefix, argv);

	read_cache_preload(&s.pathspec);
	refresh_index(&the_index, REFRESH_QUIET|REFRESH_UNMERGED, &s.pathspec, NULL, NULL);

	fd = hold_locked_index(&index_lock, 0);
	if (0 <= fd)
		update_index_if_able(&the_index, &index_lock);

	s.is_initial = get_sha1(s.reference, sha1) ? 1 : 0;
	s.ignore_submodule_arg = ignore_submodule_arg;
	wt_status_collect(&s);

	if (s.relative_paths)
		s.prefix = prefix;

	switch (status_format) {
	case STATUS_FORMAT_SHORT:
		wt_shortstatus_print(&s);
		break;
	case STATUS_FORMAT_PORCELAIN:
		wt_porcelain_print(&s);
		break;
	case STATUS_FORMAT_UNSPECIFIED:
		die("BUG: finalize_deferred_config() should have been called");
		break;
	case STATUS_FORMAT_NONE:
	case STATUS_FORMAT_LONG:
		s.verbose = verbose;
		s.ignore_submodule_arg = ignore_submodule_arg;
		wt_status_print(&s);
		break;
	}
	return 0;
}

static void print_summary(const char *prefix, const unsigned char *sha1,
			  int initial_commit)
{
	struct rev_info rev;
	struct commit *commit;
	struct strbuf format = STRBUF_INIT;
	unsigned char junk_sha1[20];
	const char *head;
	struct pretty_print_context pctx = {0};
	struct strbuf author_ident = STRBUF_INIT;
	struct strbuf committer_ident = STRBUF_INIT;

	commit = lookup_commit(sha1);
	if (!commit)
		die(_("couldn't look up newly created commit"));
	if (!commit || parse_commit(commit))
		die(_("could not parse newly created commit"));

	strbuf_addstr(&format, "format:%h] %s");

	format_commit_message(commit, "%an <%ae>", &author_ident, &pctx);
	format_commit_message(commit, "%cn <%ce>", &committer_ident, &pctx);
	if (strbuf_cmp(&author_ident, &committer_ident)) {
		strbuf_addstr(&format, "\n Author: ");
		strbuf_addbuf_percentquote(&format, &author_ident);
	}
	if (!committer_ident_sufficiently_given()) {
		strbuf_addstr(&format, "\n Committer: ");
		strbuf_addbuf_percentquote(&format, &committer_ident);
		if (advice_implicit_identity) {
			strbuf_addch(&format, '\n');
			strbuf_addstr(&format, _(implicit_ident_advice));
		}
	}
	strbuf_release(&author_ident);
	strbuf_release(&committer_ident);

	init_revisions(&rev, prefix);
	setup_revisions(0, NULL, &rev, NULL);

	rev.diff = 1;
	rev.diffopt.output_format =
		DIFF_FORMAT_SHORTSTAT | DIFF_FORMAT_SUMMARY;

	rev.verbose_header = 1;
	rev.show_root_diff = 1;
	get_commit_format(format.buf, &rev);
	rev.always_show_header = 0;
	rev.diffopt.detect_rename = 1;
	rev.diffopt.break_opt = 0;
	diff_setup_done(&rev.diffopt);

	head = resolve_ref_unsafe("HEAD", junk_sha1, 0, NULL);
	printf("[%s%s ",
		!prefixcmp(head, "refs/heads/") ?
			head + 11 :
			!strcmp(head, "HEAD") ?
				_("detached HEAD") :
				head,
		initial_commit ? _(" (root-commit)") : "");

	if (!log_tree_commit(&rev, commit)) {
		rev.always_show_header = 1;
		rev.use_terminator = 1;
		log_tree_commit(&rev, commit);
	}

	strbuf_release(&format);
}

static int git_commit_config(const char *k, const char *v, void *cb)
{
	struct wt_status *s = cb;
	int status;

	if (!strcmp(k, "commit.template"))
		return git_config_pathname(&template_file, k, v);
	if (!strcmp(k, "commit.status")) {
		include_status = git_config_bool(k, v);
		return 0;
	}
	if (!strcmp(k, "commit.cleanup"))
		return git_config_string(&cleanup_arg, k, v);

	status = git_gpg_config(k, v, NULL);
	if (status)
		return status;
	return git_status_config(k, v, s);
}

static int run_rewrite_hook(const unsigned char *oldsha1,
			    const unsigned char *newsha1)
{
	/* oldsha1 SP newsha1 LF NUL */
	static char buf[2*40 + 3];
	struct child_process proc;
	const char *argv[3];
	int code;
	size_t n;

	argv[0] = find_hook("post-rewrite");
	if (!argv[0])
		return 0;

	argv[1] = "amend";
	argv[2] = NULL;

	memset(&proc, 0, sizeof(proc));
	proc.argv = argv;
	proc.in = -1;
	proc.stdout_to_stderr = 1;

	code = start_command(&proc);
	if (code)
		return code;
	n = snprintf(buf, sizeof(buf), "%s %s\n",
		     sha1_to_hex(oldsha1), sha1_to_hex(newsha1));
	write_in_full(proc.in, buf, n);
	close(proc.in);
	return finish_command(&proc);
}

int cmd_commit(int argc, const char **argv, const char *prefix)
{
	static struct wt_status s;
	static struct option builtin_commit_options[] = {
		OPT__QUIET(&quiet, N_("suppress summary after successful commit")),
		OPT__VERBOSE(&verbose, N_("show diff in commit message template")),

		OPT_GROUP(N_("Commit message options")),
		OPT_FILENAME('F', "file", &logfile, N_("read message from file")),
		OPT_STRING(0, "author", &force_author, N_("author"), N_("override author for commit")),
		OPT_STRING(0, "date", &force_date, N_("date"), N_("override date for commit")),
		OPT_CALLBACK('m', "message", &message, N_("message"), N_("commit message"), opt_parse_m),
		OPT_STRING('c', "reedit-message", &edit_message, N_("commit"), N_("reuse and edit message from specified commit")),
		OPT_STRING('C', "reuse-message", &use_message, N_("commit"), N_("reuse message from specified commit")),
		OPT_STRING(0, "fixup", &fixup_message, N_("commit"), N_("use autosquash formatted message to fixup specified commit")),
		OPT_STRING(0, "squash", &squash_message, N_("commit"), N_("use autosquash formatted message to squash specified commit")),
		OPT_BOOLEAN(0, "reset-author", &renew_authorship, N_("the commit is authored by me now (used with -C/-c/--amend)")),
		OPT_BOOLEAN('s', "signoff", &signoff, N_("add Signed-off-by:")),
		OPT_FILENAME('t', "template", &template_file, N_("use specified template file")),
		OPT_BOOL('e', "edit", &edit_flag, N_("force edit of commit")),
		OPT_STRING(0, "cleanup", &cleanup_arg, N_("default"), N_("how to strip spaces and #comments from message")),
		OPT_BOOLEAN(0, "status", &include_status, N_("include status in commit message template")),
		{ OPTION_STRING, 'S', "gpg-sign", &sign_commit, N_("key id"),
		  N_("GPG sign commit"), PARSE_OPT_OPTARG, NULL, (intptr_t) "" },
		/* end commit message options */

		OPT_GROUP(N_("Commit contents options")),
		OPT_BOOLEAN('a', "all", &all, N_("commit all changed files")),
		OPT_BOOLEAN('i', "include", &also, N_("add specified files to index for commit")),
		OPT_BOOLEAN(0, "interactive", &interactive, N_("interactively add files")),
		OPT_BOOLEAN('p', "patch", &patch_interactive, N_("interactively add changes")),
		OPT_BOOLEAN('o', "only", &only, N_("commit only specified files")),
		OPT_BOOLEAN('n', "no-verify", &no_verify, N_("bypass pre-commit hook")),
		OPT_BOOLEAN(0, "dry-run", &dry_run, N_("show what would be committed")),
		OPT_SET_INT(0, "short", &status_format, N_("show status concisely"),
			    STATUS_FORMAT_SHORT),
		OPT_BOOL(0, "branch", &s.show_branch, N_("show branch information")),
		OPT_SET_INT(0, "porcelain", &status_format,
			    N_("machine-readable output"), STATUS_FORMAT_PORCELAIN),
		OPT_SET_INT(0, "long", &status_format,
			    N_("show status in long format (default)"),
			    STATUS_FORMAT_LONG),
		OPT_BOOLEAN('z', "null", &s.null_termination,
			    N_("terminate entries with NUL")),
		OPT_BOOLEAN(0, "amend", &amend, N_("amend previous commit")),
		OPT_BOOLEAN(0, "no-post-rewrite", &no_post_rewrite, N_("bypass post-rewrite hook")),
		{ OPTION_STRING, 'u', "untracked-files", &untracked_files_arg, N_("mode"), N_("show untracked files, optional modes: all, normal, no. (Default: all)"), PARSE_OPT_OPTARG, NULL, (intptr_t)"all" },
		/* end commit contents options */

		{ OPTION_BOOLEAN, 0, "allow-empty", &allow_empty, NULL,
		  N_("ok to record an empty change"),
		  PARSE_OPT_NOARG | PARSE_OPT_HIDDEN },
		{ OPTION_BOOLEAN, 0, "allow-empty-message", &allow_empty_message, NULL,
		  N_("ok to record a change with an empty message"),
		  PARSE_OPT_NOARG | PARSE_OPT_HIDDEN },

		OPT_END()
	};

	struct strbuf sb = STRBUF_INIT;
	struct strbuf author_ident = STRBUF_INIT;
	const char *index_file, *reflog_msg;
	char *nl, *p;
	unsigned char sha1[20];
	struct ref_lock *ref_lock;
	struct commit_list *parents = NULL, **pptr = &parents;
	struct stat statbuf;
	int allow_fast_forward = 1;
	struct commit *current_head = NULL;
	struct commit_extra_header *extra = NULL;

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(builtin_commit_usage, builtin_commit_options);

	wt_status_prepare(&s);
	gitmodules_config();
	git_config(git_commit_config, &s);
	status_format = STATUS_FORMAT_NONE; /* Ignore status.short */
	determine_whence(&s);
	s.colopts = 0;

	if (get_sha1("HEAD", sha1))
		current_head = NULL;
	else {
		current_head = lookup_commit_or_die(sha1, "HEAD");
		if (!current_head || parse_commit(current_head))
			die(_("could not parse HEAD commit"));
	}
	argc = parse_and_validate_options(argc, argv, builtin_commit_options,
					  builtin_commit_usage,
					  prefix, current_head, &s);
	if (dry_run)
		return dry_run_commit(argc, argv, prefix, current_head, &s);
	index_file = prepare_index(argc, argv, prefix, current_head, 0);

	/* Set up everything for writing the commit object.  This includes
	   running hooks, writing the trees, and interacting with the user.  */
	if (!prepare_to_commit(index_file, prefix,
			       current_head, &s, &author_ident)) {
		rollback_index_files();
		return 1;
	}

	/* Determine parents */
	reflog_msg = getenv("GIT_REFLOG_ACTION");
	if (!current_head) {
		if (!reflog_msg)
			reflog_msg = "commit (initial)";
	} else if (amend) {
		struct commit_list *c;

		if (!reflog_msg)
			reflog_msg = "commit (amend)";
		for (c = current_head->parents; c; c = c->next)
			pptr = &commit_list_insert(c->item, pptr)->next;
	} else if (whence == FROM_MERGE) {
		struct strbuf m = STRBUF_INIT;
		FILE *fp;

		if (!reflog_msg)
			reflog_msg = "commit (merge)";
		pptr = &commit_list_insert(current_head, pptr)->next;
		fp = fopen(git_path("MERGE_HEAD"), "r");
		if (fp == NULL)
			die_errno(_("could not open '%s' for reading"),
				  git_path("MERGE_HEAD"));
		while (strbuf_getline(&m, fp, '\n') != EOF) {
			struct commit *parent;

			parent = get_merge_parent(m.buf);
			if (!parent)
				die(_("Corrupt MERGE_HEAD file (%s)"), m.buf);
			pptr = &commit_list_insert(parent, pptr)->next;
		}
		fclose(fp);
		strbuf_release(&m);
		if (!stat(git_path("MERGE_MODE"), &statbuf)) {
			if (strbuf_read_file(&sb, git_path("MERGE_MODE"), 0) < 0)
				die_errno(_("could not read MERGE_MODE"));
			if (!strcmp(sb.buf, "no-ff"))
				allow_fast_forward = 0;
		}
		if (allow_fast_forward)
			parents = reduce_heads(parents);
	} else {
		if (!reflog_msg)
			reflog_msg = (whence == FROM_CHERRY_PICK)
					? "commit (cherry-pick)"
					: "commit";
		pptr = &commit_list_insert(current_head, pptr)->next;
	}

	/* Finally, get the commit message */
	strbuf_reset(&sb);
	if (strbuf_read_file(&sb, git_path(commit_editmsg), 0) < 0) {
		int saved_errno = errno;
		rollback_index_files();
		die(_("could not read commit message: %s"), strerror(saved_errno));
	}

	/* Truncate the message just before the diff, if any. */
	if (verbose) {
		p = strstr(sb.buf, "\ndiff --git ");
		if (p != NULL)
			strbuf_setlen(&sb, p - sb.buf + 1);
	}

	if (cleanup_mode != CLEANUP_NONE)
		stripspace(&sb, cleanup_mode == CLEANUP_ALL);
	if (template_untouched(&sb) && !allow_empty_message) {
		rollback_index_files();
		fprintf(stderr, _("Aborting commit; you did not edit the message.\n"));
		exit(1);
	}
	if (message_is_empty(&sb) && !allow_empty_message) {
		rollback_index_files();
		fprintf(stderr, _("Aborting commit due to empty commit message.\n"));
		exit(1);
	}

	if (amend) {
		const char *exclude_gpgsig[2] = { "gpgsig", NULL };
		extra = read_commit_extra_headers(current_head, exclude_gpgsig);
	} else {
		struct commit_extra_header **tail = &extra;
		append_merge_tag_headers(parents, &tail);
	}

	if (commit_tree_extended(&sb, active_cache_tree->sha1, parents, sha1,
				 author_ident.buf, sign_commit, extra)) {
		rollback_index_files();
		die(_("failed to write commit object"));
	}
	strbuf_release(&author_ident);
	free_commit_extra_headers(extra);

	ref_lock = lock_any_ref_for_update("HEAD",
					   !current_head
					   ? NULL
					   : current_head->object.sha1,
					   0);

	nl = strchr(sb.buf, '\n');
	if (nl)
		strbuf_setlen(&sb, nl + 1 - sb.buf);
	else
		strbuf_addch(&sb, '\n');
	strbuf_insert(&sb, 0, reflog_msg, strlen(reflog_msg));
	strbuf_insert(&sb, strlen(reflog_msg), ": ", 2);

	if (!ref_lock) {
		rollback_index_files();
		die(_("cannot lock HEAD ref"));
	}
	if (write_ref_sha1(ref_lock, sha1, sb.buf) < 0) {
		rollback_index_files();
		die(_("cannot update HEAD ref"));
	}

	unlink(git_path("CHERRY_PICK_HEAD"));
	unlink(git_path("REVERT_HEAD"));
	unlink(git_path("MERGE_HEAD"));
	unlink(git_path("MERGE_MSG"));
	unlink(git_path("MERGE_MODE"));
	unlink(git_path("SQUASH_MSG"));

	if (commit_index_files())
		die (_("Repository has been updated, but unable to write\n"
		     "new_index file. Check that disk is not full or quota is\n"
		     "not exceeded, and then \"git reset HEAD\" to recover."));

	rerere(0);
	run_hook(get_index_file(), "post-commit", NULL);
	if (amend && !no_post_rewrite) {
		struct notes_rewrite_cfg *cfg;
		cfg = init_copy_notes_for_rewrite("amend");
		if (cfg) {
			/* we are amending, so current_head is not NULL */
			copy_note_for_rewrite(cfg, current_head->object.sha1, sha1);
			finish_copy_notes_for_rewrite(cfg, "Notes added by 'git commit --amend'");
		}
		run_rewrite_hook(current_head->object.sha1, sha1);
	}
	if (!quiet)
		print_summary(prefix, sha1, !current_head);

	return 0;
}
