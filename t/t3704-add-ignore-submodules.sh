#!/bin/sh
#
# Copyright (c) 2014 Ronald Weiss
#

test_description='Test of git add with ignoring submodules'

. ./test-lib.sh

test_expect_success 'create dirty submodule' '
	test_create_repo sm && (
		cd sm &&
		>foo &&
		git add foo &&
		git commit -m "Add foo"
	) &&
	git submodule add ./sm &&
	git commit -m "Add sm" && (
		cd sm &&
		echo bar >> foo &&
		git add foo &&
		git commit -m "Update foo"
	)
'

test_expect_success 'add --ignore-submodules ignores submodule' '
	git reset &&
	git add -u --ignore-submodules &&
	git diff --cached --exit-code --ignore-submodules=none
'

test_expect_success 'add --ignore-submodules=all ignores submodule' '
	git reset &&
	git add -u --ignore-submodules=all &&
	git diff --cached --exit-code --ignore-submodules=none
'

test_expect_success 'add --ignore-submodules=none overrides ignore=all from config' '
	git reset &&
	git config submodule.sm.ignore all &&
	git add -u --ignore-submodules=none &&
	test_must_fail git diff --cached --exit-code --ignore-submodules=none
'

test_done
