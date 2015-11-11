#!/bin/sh

test_description='revert can handle submodules'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-submodule-update.sh

# Create a revert that moves from HEAD (including any test modifications to
# the work tree) to $1 by first checking out $1 and reverting it. Reverting
# the revert is the transition we test for. We tar the current work tree
# first so we can restore the work tree test setup after doing the checkout
# and revert.  We test here that the restored work tree content is identical
# to that at the beginning. The last revert is then tested by the framework.
git_revert () {
	git status -su >expect &&
	ls -1pR * >>expect &&
	tar czf "$TRASH_DIRECTORY/tmp.tgz" * &&
	git checkout "$1" &&
	git revert HEAD &&
	rm -rf * &&
	tar xzf "$TRASH_DIRECTORY/tmp.tgz" &&
	git status -su >actual &&
	ls -1pR * >>actual &&
	test_cmp expect actual &&
	git revert HEAD
}

KNOWN_FAILURE_CHERRY_PICK_SEES_EMPTY_COMMIT=1
KNOWN_FAILURE_NOFF_MERGE_DOESNT_CREATE_EMPTY_SUBMODULE_DIR=1
test_submodule_switch "git_revert"

git_revert_recursive () {
	git status -su >expect &&
	ls -1pR * >>expect &&
	tar czf "$TRASH_DIRECTORY/tmp.tgz" * &&
	git checkout "$1" &&
	git revert HEAD &&
	rm -rf * &&
	tar xzf "$TRASH_DIRECTORY/tmp.tgz" &&
	git status -su >actual &&
	ls -1pR * >>actual &&
	test_cmp expect actual &&
	git revert --recurse-submodules HEAD
}

KNOWN_FAILURE_CHERRY_PICK_SEES_EMPTY_COMMIT=
KNOWN_FAILURE_NOFF_MERGE_DOESNT_CREATE_EMPTY_SUBMODULE_DIR=
test_submodule_recursive_switch "git_revert_recursive"

test_done
