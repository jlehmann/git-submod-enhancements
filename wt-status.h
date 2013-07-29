#ifndef STATUS_H
#define STATUS_H

#include <stdio.h>
#include "string-list.h"
#include "color.h"

enum color_wt_status {
	WT_STATUS_HEADER = 0,
	WT_STATUS_UPDATED,
	WT_STATUS_CHANGED,
	WT_STATUS_UNTRACKED,
	WT_STATUS_NOBRANCH,
	WT_STATUS_UNMERGED,
	WT_STATUS_LOCAL_BRANCH,
	WT_STATUS_REMOTE_BRANCH,
	WT_STATUS_ONBRANCH,
	WT_STATUS_MAXSLOT
};

enum untracked_status_type {
	SHOW_NO_UNTRACKED_FILES,
	SHOW_NORMAL_UNTRACKED_FILES,
	SHOW_ALL_UNTRACKED_FILES
};

/* from where does this commit originate */
enum commit_whence {
	FROM_COMMIT,     /* normal */
	FROM_MERGE,      /* commit came from merge */
	FROM_CHERRY_PICK /* commit came from cherry-pick */
};

struct wt_status_change_data {
	int worktree_status;
	int index_status;
	int stagemask;
	char *head_path;
	unsigned dirty_submodule       : 2;
	unsigned new_submodule_commits : 1;
};

struct wt_status {
	int is_initial;
	char *branch;
	const char *reference;
	struct pathspec pathspec;
	int verbose;
	int amend;
	enum commit_whence whence;
	int nowarn;
	int use_color;
	int relative_paths;
	int submodule_summary;
	int show_ignored_files;
	enum untracked_status_type show_untracked_files;
	const char *ignore_submodule_arg;
	char color_palette[WT_STATUS_MAXSLOT][COLOR_MAXLEN];
	unsigned colopts;
	int null_termination;
	int show_branch;

	/* These are computed during processing of the individual sections */
	int commitable;
	int workdir_dirty;
	const char *index_file;
	FILE *fp;
	const char *prefix;
	struct string_list change;
	struct string_list untracked;
	struct string_list ignored;
	uint32_t untracked_in_ms;
};

struct wt_status_state {
	int merge_in_progress;
	int am_in_progress;
	int am_empty_patch;
	int rebase_in_progress;
	int rebase_interactive_in_progress;
	int cherry_pick_in_progress;
	int bisect_in_progress;
	int revert_in_progress;
	char *branch;
	char *onto;
	char *detached_from;
	unsigned char detached_sha1[20];
	unsigned char revert_head_sha1[20];
};

void wt_status_prepare(struct wt_status *s);
void wt_status_print(struct wt_status *s);
void wt_status_collect(struct wt_status *s);
void wt_status_get_state(struct wt_status_state *state, int get_detached_from);

void wt_shortstatus_print(struct wt_status *s);
void wt_porcelain_print(struct wt_status *s);

__attribute__((format (printf, 3, 4)))
void status_printf_ln(struct wt_status *s, const char *color, const char *fmt, ...);
__attribute__((format (printf, 3, 4)))
void status_printf(struct wt_status *s, const char *color, const char *fmt, ...);

#endif /* STATUS_H */
