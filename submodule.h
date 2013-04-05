#ifndef SUBMODULE_H
#define SUBMODULE_H

struct diff_options;
struct argv_array;
struct option;

enum {
	RECURSE_SUBMODULES_ON_DEMAND = -1,
	RECURSE_SUBMODULES_OFF = 0,
	RECURSE_SUBMODULES_DEFAULT = 1,
	RECURSE_SUBMODULES_ON = 2
};

int is_staging_gitmodules_ok(void);
int update_path_in_gitmodules(const char *oldpath, const char *newpath);
int remove_path_from_gitmodules(const char *path);
void stage_updated_gitmodules(void);
void set_diffopt_flags_from_submodule_config(struct diff_options *diffopt,
		const char *path);
int submodule_config(const char *var, const char *value, void *cb);
void gitmodules_config(void);
int parse_submodule_config_option(const char *var, const char *value);
void handle_ignore_submodules_arg(struct diff_options *diffopt, const char *);
int parse_fetch_recurse_submodules_arg(const char *opt, const char *arg);
int parse_update_recurse_submodules_arg(const char *opt, const char *arg);
int option_parse_update_submodules(const struct option *opt,
		const char *arg, int unset);
int submodule_needs_update(const char *path);
int populate_submodule(const char *path, unsigned char sha1[20], int force);
int depopulate_submodule(const char *path);
int update_submodule(const char *path, const unsigned char sha1[20], int force);
void show_submodule_summary(FILE *f, const char *path,
		const char *line_prefix,
		unsigned char one[20], unsigned char two[20],
		unsigned dirty_submodule, const char *meta,
		const char *del, const char *add, const char *reset);
void set_config_fetch_recurse_submodules(int value);
void set_config_update_recurse_submodules(int default_value, int option_value);
void check_for_new_submodule_commits(unsigned char new_sha1[20]);
int fetch_populated_submodules(const struct argv_array *options,
			       const char *prefix, int command_line_option,
			       int quiet);
int is_submodule_populated(const char *path);
unsigned is_submodule_modified(const char *path, int ignore_untracked);
int submodule_uses_gitfile(const char *path);
int ok_to_remove_submodule(const char *path);
unsigned is_submodule_checkout_safe(const char *path, const unsigned char sha1[20]);
int merge_submodule(unsigned char result[20], const char *path, const unsigned char base[20],
		    const unsigned char a[20], const unsigned char b[20], int search);
int find_unpushed_submodules(unsigned char new_sha1[20], const char *remotes_name,
		struct string_list *needs_pushing);
int push_unpushed_submodules(unsigned char new_sha1[20], const char *remotes_name);
void connect_work_tree_and_git_dir(const char *work_tree, const char *git_dir);

#endif
