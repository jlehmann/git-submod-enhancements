#ifndef BRANCH_H
#define BRANCH_H

/* Functions for acting on the information about branches. */

/*
 * Creates a new branch, where:
 *
 * - head is the branch currently checked out;
 * - name is the new branch name;
 * - start_name is the name of a commit that the new branch should start at
 *   (could be another branch or a remote-tracking branch, in which case
 *   track---see below---may also trigger);
 * - flags indicates overwriting an existing branch and/or overwriting the
 *   current branch is allowed;
 * - reflog creates a reflog for the branch; and
 * - track causes the new branch to be configured to merge the remote branch
 *   that start_name is a tracking branch for (if any).
 */
#define CREATE_BRANCH_UPDATE_OK 01
#define CREATE_BRANCH_UPDATE_CURRENT_OK 02
void create_branch(const char *head, const char *name, const char *start_name,
		   int flags, int reflog, enum branch_track track);

/*
 * Remove information about the state of working on the current
 * branch. (E.g., MERGE_HEAD)
 */
void remove_branch_state(void);

/*
 * Configure local branch "local" to merge remote branch "remote"
 * taken from origin "origin".
 */
#define BRANCH_CONFIG_VERBOSE 01
extern void install_branch_config(int flag, const char *local, const char *origin, const char *remote);

#endif
