#!/bin/sh

test_description='checkout switching away from an invalid branch'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-checkout.sh

test_expect_success 'setup' '
	echo hello >world &&
	git add world &&
	git commit -m initial
'

test_expect_success 'checkout should not start branch from a tree' '
	checkout_must_fail -b newbranch master^{tree}
'

test_expect_success 'checkout master from invalid HEAD' '
	echo $_z40 >.git/HEAD &&
	checkout_must_succeed master --
'

test_done
