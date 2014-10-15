#!/bin/sh

test_description='pull can handle submodules'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-submodule-update.sh

reset_branch_to_HEAD () {
	git branch -D "$1" &&
	git checkout -b "$1" HEAD &&
	git branch --set-upstream-to="origin/$1" "$1"
}

git_pull () {
	reset_branch_to_HEAD "$1" &&
	git pull
}

# pulls without conflicts
test_submodule_switch "git_pull"

git_pull_ff () {
	reset_branch_to_HEAD "$1" &&
	git pull --ff
}

test_submodule_switch "git_pull_ff"

git_pull_ff_only () {
	reset_branch_to_HEAD "$1" &&
	git pull --ff-only
}

test_submodule_switch "git_pull_ff_only"

git_pull_noff () {
	reset_branch_to_HEAD "$1" &&
	git pull --no-ff
}

KNOWN_FAILURE_NOFF_MERGE_DOESNT_CREATE_EMPTY_SUBMODULE_DIR=1
KNOWN_FAILURE_NOFF_MERGE_ATTEMPTS_TO_MERGE_REMOVED_SUBMODULE_FILES=1
test_submodule_switch "git_pull_noff"

KNOWN_FAILURE_NOFF_MERGE_DOESNT_CREATE_EMPTY_SUBMODULE_DIR=
KNOWN_FAILURE_NOFF_MERGE_ATTEMPTS_TO_MERGE_REMOVED_SUBMODULE_FILES=

git_pull_recursive () {
	reset_branch_to_HEAD "$1" &&
	git pull --recurse-submodules
}

# pulls without conflicts
test_submodule_recursive_switch "git_pull_recursive"

git_pull_recursive_ff () {
	reset_branch_to_HEAD "$1" &&
	git pull --recurse-submodules --ff
}

test_submodule_recursive_switch "git_pull_recursive_ff"

git_pull_recursive_ff_only () {
	reset_branch_to_HEAD "$1" &&
	git pull --recurse-submodules --ff-only
}

test_submodule_recursive_switch "git_pull_recursive_ff_only"

git_pull_recursive_noff () {
	reset_branch_to_HEAD "$1" &&
	git pull --recurse-submodules --no-ff
}

test_submodule_recursive_switch "git_pull_recursive_noff"

test_done
