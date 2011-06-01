#!/bin/sh

test_description='checkout and pathspecs/refspecs ambiguities'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-checkout.sh

test_expect_success 'setup' '
	echo hello >world &&
	echo hello >all &&
	git add all world &&
	git commit -m initial &&
	git branch world
'

test_expect_success 'reference must be a tree' '
	checkout_must_fail $(git hash-object ./all) --
'

test_expect_success 'branch switching' '
	test "refs/heads/master" = "$(git symbolic-ref HEAD)" &&
	checkout_must_succeed world -- &&
	test "refs/heads/world" = "$(git symbolic-ref HEAD)"
'

test_expect_success 'checkout world from the index' '
	echo bye > world &&
	checkout_must_succeed -- world &&
	git diff --exit-code --quiet
'

test_expect_success 'non ambiguous call' '
	checkout_must_succeed all
'

test_expect_success 'allow the most common case' '
	checkout_must_succeed world &&
	test "refs/heads/world" = "$(git symbolic-ref HEAD)"
'

test_expect_success 'check ambiguity' '
	checkout_must_fail world all
'

test_expect_success 'disambiguate checking out from a tree-ish' '
	echo bye > world &&
	checkout_must_succeed world -- world &&
	git diff --exit-code --quiet
'

test_done
