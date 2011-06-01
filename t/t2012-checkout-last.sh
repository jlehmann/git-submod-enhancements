#!/bin/sh

test_description='checkout can switch to last branch and merge base'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-checkout.sh

test_expect_success 'setup' '
	echo hello >world &&
	git add world &&
	git commit -m initial &&
	git branch other &&
	echo "hello again" >>world &&
	git add world &&
	git commit -m second
'

test_expect_success '"checkout -" does not work initially' '
	checkout_must_fail -
'

test_expect_success 'first branch switch' '
	checkout_must_succeed other
'

test_expect_success '"checkout -" switches back' '
	checkout_must_succeed - &&
	test "z$(git symbolic-ref HEAD)" = "zrefs/heads/master"
'

test_expect_success '"checkout -" switches forth' '
	checkout_must_succeed - &&
	test "z$(git symbolic-ref HEAD)" = "zrefs/heads/other"
'

test_expect_success 'detach HEAD' '
	checkout_must_succeed $(git rev-parse HEAD)
'

test_expect_success '"checkout -" attaches again' '
	checkout_must_succeed - &&
	test "z$(git symbolic-ref HEAD)" = "zrefs/heads/other"
'

test_expect_success '"checkout -" detaches again' '
	checkout_must_succeed - &&
	test "z$(git rev-parse HEAD)" = "z$(git rev-parse other)" &&
	test_must_fail git symbolic-ref HEAD
'

test_expect_success 'more switches' '
	for i in 16 15 14 13 12 11 10 9 8 7 6 5 4 3 2 1
	do
		checkout_must_succeed -b branch$i
	done
'

more_switches () {
	for i in 16 15 14 13 12 11 10 9 8 7 6 5 4 3 2 1
	do
		checkout_must_succeed branch$i
	done
}

test_expect_success 'switch to the last' '
	more_switches &&
	checkout_must_succeed @{-1} &&
	test "z$(git symbolic-ref HEAD)" = "zrefs/heads/branch2"
'

test_expect_success 'switch to second from the last' '
	more_switches &&
	checkout_must_succeed @{-2} &&
	test "z$(git symbolic-ref HEAD)" = "zrefs/heads/branch3"
'

test_expect_success 'switch to third from the last' '
	more_switches &&
	checkout_must_succeed @{-3} &&
	test "z$(git symbolic-ref HEAD)" = "zrefs/heads/branch4"
'

test_expect_success 'switch to fourth from the last' '
	more_switches &&
	checkout_must_succeed @{-4} &&
	test "z$(git symbolic-ref HEAD)" = "zrefs/heads/branch5"
'

test_expect_success 'switch to twelfth from the last' '
	more_switches &&
	checkout_must_succeed @{-12} &&
	test "z$(git symbolic-ref HEAD)" = "zrefs/heads/branch13"
'

test_expect_success 'merge base test setup' '
	checkout_must_succeed -b another other &&
	echo "hello again" >>world &&
	git add world &&
	git commit -m third
'

test_expect_success 'another...master' '
	checkout_must_succeed another &&
	checkout_must_succeed another...master &&
	test "z$(git rev-parse --verify HEAD)" = "z$(git rev-parse --verify master^)"
'

test_expect_success '...master' '
	checkout_must_succeed another &&
	checkout_must_succeed ...master &&
	test "z$(git rev-parse --verify HEAD)" = "z$(git rev-parse --verify master^)"
'

test_expect_success 'master...' '
	checkout_must_succeed another &&
	checkout_must_succeed master... &&
	test "z$(git rev-parse --verify HEAD)" = "z$(git rev-parse --verify master^)"
'

test_done
