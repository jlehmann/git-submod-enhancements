#!/bin/sh
#
# Copyright (c) 2010 Erick Mattos
#

test_description='git checkout --orphan

Main Tests for --orphan functionality.'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-checkout.sh

TEST_FILE=foo

test_expect_success 'Setup' '
	echo "Initial" >"$TEST_FILE" &&
	git add "$TEST_FILE" &&
	git commit -m "First Commit" &&
	test_tick &&
	echo "State 1" >>"$TEST_FILE" &&
	git add "$TEST_FILE" &&
	test_tick &&
	git commit -m "Second Commit"
'

test_expect_success '--orphan creates a new orphan branch from HEAD' '
	checkout_must_succeed --orphan alpha &&
	test_must_fail git rev-parse --verify HEAD &&
	test "refs/heads/alpha" = "$(git symbolic-ref HEAD)" &&
	test_tick &&
	git commit -m "Third Commit" &&
	test_must_fail git rev-parse --verify HEAD^ &&
	git diff-tree --quiet master alpha
'

test_expect_success '--orphan creates a new orphan branch from <start_point>' '
	checkout_must_succeed master &&
	checkout_must_succeed --orphan beta master^ &&
	test_must_fail git rev-parse --verify HEAD &&
	test "refs/heads/beta" = "$(git symbolic-ref HEAD)" &&
	test_tick &&
	git commit -m "Fourth Commit" &&
	test_must_fail git rev-parse --verify HEAD^ &&
	git diff-tree --quiet master^ beta
'

test_expect_success '--orphan must be rejected with -b' '
	checkout_must_succeed master &&
	checkout_must_fail --orphan new -b newer &&
	test refs/heads/master = "$(git symbolic-ref HEAD)"
'

test_expect_success '--orphan must be rejected with -t' '
	checkout_must_succeed master &&
	checkout_must_fail --orphan new -t master &&
	test refs/heads/master = "$(git symbolic-ref HEAD)"
'

test_expect_success '--orphan ignores branch.autosetupmerge' '
	checkout_must_succeed master &&
	git config branch.autosetupmerge always &&
	checkout_must_succeed --orphan gamma &&
	test -z "$(git config branch.gamma.merge)" &&
	test refs/heads/gamma = "$(git symbolic-ref HEAD)" &&
	test_must_fail git rev-parse --verify HEAD^
'

test_expect_success '--orphan makes reflog by default' '
	checkout_must_succeed master &&
	git config --unset core.logAllRefUpdates &&
	checkout_must_succeed --orphan delta &&
	test_must_fail git rev-parse --verify delta@{0} &&
	git commit -m Delta &&
	git rev-parse --verify delta@{0}
'

test_expect_success '--orphan does not make reflog when core.logAllRefUpdates = false' '
	checkout_must_succeed master &&
	git config core.logAllRefUpdates false &&
	checkout_must_succeed --orphan epsilon &&
	test_must_fail git rev-parse --verify epsilon@{0} &&
	git commit -m Epsilon &&
	test_must_fail git rev-parse --verify epsilon@{0}
'

test_expect_success '--orphan with -l makes reflog when core.logAllRefUpdates = false' '
	checkout_must_succeed master &&
	checkout_must_succeed -l --orphan zeta &&
	test_must_fail git rev-parse --verify zeta@{0} &&
	git commit -m Zeta &&
	git rev-parse --verify zeta@{0}
'

test_expect_success 'giving up --orphan not committed when -l and core.logAllRefUpdates = false deletes reflog' '
	checkout_must_succeed master &&
	checkout_must_succeed -l --orphan eta &&
	test_must_fail git rev-parse --verify eta@{0} &&
	checkout_must_succeed master &&
	test_must_fail git rev-parse --verify eta@{0}
'

test_expect_success '--orphan is rejected with an existing name' '
	checkout_must_succeed master &&
	checkout_must_fail --orphan master &&
	test refs/heads/master = "$(git symbolic-ref HEAD)"
'

test_expect_success '--orphan refuses to switch if a merge is needed' '
	checkout_must_succeed master &&
	git reset --hard &&
	echo local >>"$TEST_FILE" &&
	cat "$TEST_FILE" >"$TEST_FILE.saved" &&
	checkout_must_fail --orphan new master^ &&
	test refs/heads/master = "$(git symbolic-ref HEAD)" &&
	test_cmp "$TEST_FILE" "$TEST_FILE.saved" &&
	git diff-index --quiet --cached HEAD &&
	git reset --hard
'

test_expect_success 'cannot --detach on an unborn branch' '
	git checkout master &&
	git checkout --orphan new &&
	test_must_fail git checkout --detach
'

test_done
