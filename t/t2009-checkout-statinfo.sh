#!/bin/sh

test_description='checkout should leave clean stat info'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-checkout.sh

test_expect_success 'setup' '

	echo hello >world &&
	git update-index --add world &&
	git commit -m initial &&
	git branch side &&
	echo goodbye >world &&
	git update-index --add world &&
	git commit -m second

'

test_expect_success 'branch switching' '

	git reset --hard &&
	test "$(git diff-files --raw)" = "" &&

	checkout_must_succeed master &&
	test "$(git diff-files --raw)" = "" &&

	checkout_must_succeed side &&
	test "$(git diff-files --raw)" = "" &&

	checkout_must_succeed master &&
	test "$(git diff-files --raw)" = ""

'

test_expect_success 'path checkout' '

	git reset --hard &&
	test "$(git diff-files --raw)" = "" &&

	checkout_must_succeed master world &&
	test "$(git diff-files --raw)" = "" &&

	checkout_must_succeed side world &&
	test "$(git diff-files --raw)" = "" &&

	checkout_must_succeed master world &&
	test "$(git diff-files --raw)" = ""

'

test_done

