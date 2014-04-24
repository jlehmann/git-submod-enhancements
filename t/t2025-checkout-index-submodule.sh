#!/bin/sh

test_description='checkout-index can handle submodules'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-submodule-update.sh

# We have to read the commit to check out into the index first before
# checkout-index -a can check it out
checkout_index () {
	git ls-files | xargs rm -r  &&
	git read-tree "$1" &&
	git checkout-index -a -u
}

# TODO: fix the 5 failures from the next test
# - "added submodule doesn't remove untracked unignored file with same name" fails with "sub1 already exists, no checkout"
# - "removed submodule leaves submodule directory and its contents in place" fails ?
# - "removed submodule leaves submodule containing a .git directory alone" fails ?
# - "modified submodule does not update submodule work tree" fails because "Submodule sub1 is not populated"
# - "modified submodule does not update submodule work tree to invalid commit" fails because "Submodule sub1 is not populated"
KNOWN_FAILURE_NOFF_MERGE_ATTEMPTS_TO_MERGE_REMOVED_SUBMODULE_FILES=1
#test_submodule_switch checkout_index

# We have to read the commit to check out into the index first before
# checkout-index -a -f can check it out
checkout_index_forced () {
	git read-tree "$1" &&
	git checkout-index -a -u -f
}

# TODO: fix failure from the next test
# - "replace directory with submodule" fails because sub1 is not empty
#test_submodule_forced_switch checkout_index_forced

test_done
