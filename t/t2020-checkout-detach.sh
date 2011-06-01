#!/bin/sh

test_description='checkout into detached HEAD state'
. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-checkout.sh

check_detached () {
	test_must_fail git symbolic-ref -q HEAD >/dev/null
}

check_not_detached () {
	git symbolic-ref -q HEAD >/dev/null
}

PREV_HEAD_DESC='Previous HEAD position was'
check_orphan_warning() {
	test_i18ngrep "you are leaving $2 behind" "$1" &&
	test_i18ngrep ! "$PREV_HEAD_DESC" "$1"
}
check_no_orphan_warning() {
	test_i18ngrep ! "you are leaving .* commit.*behind" "$1" &&
	test_i18ngrep "$PREV_HEAD_DESC" "$1"
}

reset () {
	git checkout master &&
	check_not_detached
}

test_expect_success 'setup' '
	test_commit one &&
	test_commit two &&
	test_commit three && git tag -d three &&
	test_commit four && git tag -d four &&
	git branch branch &&
	git tag tag
'

test_expect_success 'checkout branch does not detach' '
	reset &&
	checkout_must_succeed branch &&
	check_not_detached
'

test_expect_success 'checkout tag detaches' '
	reset &&
	checkout_must_succeed tag &&
	check_detached
'

test_expect_success 'checkout branch by full name detaches' '
	reset &&
	checkout_must_succeed refs/heads/branch &&
	check_detached
'

test_expect_success 'checkout non-ref detaches' '
	reset &&
	checkout_must_succeed branch^ &&
	check_detached
'

test_expect_success 'checkout ref^0 detaches' '
	reset &&
	checkout_must_succeed branch^0 &&
	check_detached
'

test_expect_success 'checkout --detach detaches' '
	reset &&
	checkout_must_succeed --detach branch &&
	check_detached
'

test_expect_success 'checkout --detach without branch name' '
	reset &&
	checkout_must_succeed --detach &&
	check_detached
'

test_expect_success 'checkout --detach errors out for non-commit' '
	reset &&
	checkout_must_fail --detach one^{tree} &&
	check_not_detached
'

test_expect_success 'checkout --detach errors out for extra argument' '
	reset &&
	checkout_must_succeed master &&
	checkout_must_fail --detach tag one.t &&
	check_not_detached
'

test_expect_success 'checkout --detached and -b are incompatible' '
	reset &&
	checkout_must_fail --detach -b newbranch tag &&
	check_not_detached
'

test_expect_success 'checkout --detach moves HEAD' '
	reset &&
	checkout_must_succeed one &&
	checkout_must_succeed --detach two &&
	git diff --exit-code HEAD &&
	git diff --exit-code two
'

test_expect_success 'checkout warns on orphan commits' '
	reset &&
	checkout_must_succeed --detach two &&
	echo content >orphan &&
	git add orphan &&
	git commit -a -m orphan1 &&
	echo new content >orphan &&
	git commit -a -m orphan2 &&
	orphan2=$(git rev-parse HEAD) &&
	checkout_must_succeed master 2>stderr
'

test_expect_success 'checkout warns on orphan commits: output' '
	check_orphan_warning stderr "2 commits"
'

test_expect_success 'checkout warns orphaning 1 of 2 commits' '
	git checkout "$orphan2" &&
	git checkout HEAD^ 2>stderr
'

test_expect_success 'checkout warns orphaning 1 of 2 commits: output' '
	check_orphan_warning stderr "1 commit"
'

test_expect_success 'checkout does not warn leaving ref tip' '
	reset &&
	checkout_must_succeed --detach two &&
	checkout_must_succeed master 2>stderr
'

test_expect_success 'checkout does not warn leaving ref tip' '
	check_no_orphan_warning stderr
'

test_expect_success 'checkout does not warn leaving reachable commit' '
	reset &&
	checkout_must_succeed --detach HEAD^ &&
	checkout_must_succeed master 2>stderr
'

test_expect_success 'checkout does not warn leaving reachable commit' '
	check_no_orphan_warning stderr
'

cat >expect <<'EOF'
Your branch is behind 'master' by 1 commit, and can be fast-forwarded.
  (use "git pull" to update your local branch)
EOF
test_expect_success 'tracking count is accurate after orphan check' '
	reset &&
	git branch child master^ &&
	git config branch.child.remote . &&
	git config branch.child.merge refs/heads/master &&
	checkout_must_succeed child^ &&
	checkout_must_succeed child >stdout &&
	test_i18ncmp expect stdout
'

test_done
