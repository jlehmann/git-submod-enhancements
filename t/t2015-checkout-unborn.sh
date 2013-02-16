#!/bin/sh

test_description='checkout from unborn branch'
. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-checkout.sh

test_expect_success 'setup' '
	mkdir parent &&
	(cd parent &&
	 git init &&
	 echo content >file &&
	 git add file &&
	 git commit -m base
	) &&
	git fetch parent master:origin
'

test_expect_success 'checkout from unborn preserves untracked files' '
	echo precious >expect &&
	echo precious >file &&
	checkout_must_fail -b new origin &&
	test_cmp expect file
'

test_expect_success 'checkout from unborn preserves index contents' '
	echo precious >expect &&
	echo precious >file &&
	git add file &&
	checkout_must_fail -b new origin &&
	test_cmp expect file &&
	git show :file >file &&
	test_cmp expect file
'

test_expect_success 'checkout from unborn merges identical index contents' '
	echo content >file &&
	git add file &&
	checkout_must_succeed -b new origin
'

test_expect_success 'checking out another branch from unborn state' '
	git checkout --orphan newroot &&
	git checkout -b anothername &&
	test_must_fail git show-ref --verify refs/heads/newroot &&
	git symbolic-ref HEAD >actual &&
	echo refs/heads/anothername >expect &&
	test_cmp expect actual
'

test_expect_success 'checking out in a newly created repo' '
	test_create_repo empty &&
	(
		cd empty &&
		git symbolic-ref HEAD >expect &&
		test_must_fail git checkout &&
		git symbolic-ref HEAD >actual &&
		test_cmp expect actual
	)
'

test_done
