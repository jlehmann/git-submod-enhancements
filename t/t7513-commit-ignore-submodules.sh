#!/bin/sh
#
# Copyright (c) 2014 Ronald Weiss
#

test_description='Test of git commit --ignore-submodules'

. ./test-lib.sh

test_expect_success 'create submodule' '
	test_create_repo sm && (
		cd sm &&
		>foo &&
		git add foo &&
		git commit -m "Add foo"
	) &&
	git submodule add ./sm &&
	git commit -m "Add sm"
'

update_sm () {
	(cd sm &&
		echo bar >> foo &&
		git add foo &&
		git commit -m "Updated foo"
	)
}

test_expect_success 'commit -a --ignore-submodules=all ignores dirty submodule' '
	update_sm &&
	test_must_fail git commit -a --ignore-submodules=all -m "Update sm"
'

test_expect_success 'commit -a --ignore-submodules=none overrides ignore=all setting' '
	update_sm &&
	git config submodule.sm.ignore all &&
	git commit -a --ignore-submodules=none -m "Update sm" &&
	git diff --exit-code --ignore-submodules=none &&
	git diff --cached --exit-code --ignore-submodules=none
'

test_expect_success 'commit --ignore-submodules status of submodule with untracked content' '
	GIT_EDITOR=cat &&
	export GIT_EDITOR &&
	echo untracked > sm/untracked &&

	test_might_fail git commit --ignore-submodules=none > output &&
	test_i18ngrep modified output &&

	test_might_fail git commit --ignore-submodules=untracked > output &&
	test_must_fail test_i18ngrep modified output &&

	test_might_fail git commit --ignore-submodules=dirty > output &&
	test_must_fail test_i18ngrep modified output &&

	test_might_fail git commit --ignore-submodules=all > output &&
	test_must_fail test_i18ngrep modified output
'

test_expect_success 'commit --ignore-submodules status of dirty submodule' '
	GIT_EDITOR=cat &&
	export GIT_EDITOR &&
	echo dirty > sm/foo &&

	test_might_fail git commit --ignore-submodules=none > output &&
	test_i18ngrep modified output &&

	test_might_fail git commit --ignore-submodules=untracked > output &&
	test_i18ngrep modified output &&

	test_might_fail git commit --ignore-submodules=dirty > output &&
	test_must_fail test_i18ngrep modified output &&

	test_might_fail git commit --ignore-submodules=all > output &&
	test_must_fail test_i18ngrep modified output
'

test_done
