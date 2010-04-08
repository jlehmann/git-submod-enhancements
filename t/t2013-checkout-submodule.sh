#!/bin/sh

test_description='checkout can handle submodules'

. ./test-lib.sh

test_expect_success 'setup' '
	mkdir submodule &&
	(cd submodule &&
	 git init &&
	 test_commit first) &&
	git add submodule &&
	test_tick &&
	git commit -m superproject &&
	(cd submodule &&
	 test_commit second) &&
	git add submodule &&
	test_tick &&
	git commit -m updated.superproject
'

test_expect_success '"reset <submodule>" updates the index' '
	git update-index --refresh &&
	git diff-files --quiet &&
	git diff-index --quiet --cached HEAD &&
	test_must_fail git reset HEAD^ submodule &&
	test_must_fail git diff-files --quiet &&
	git reset submodule &&
	git diff-files --quiet
'

test_expect_success '"checkout --ignore-submodules <submodule>" updates the index only' '
	git update-index --refresh &&
	git diff-files --quiet &&
	git diff-index --quiet --cached HEAD &&
	git checkout --ignore-submodules HEAD^ submodule &&
	test_must_fail git diff-files --quiet &&
	git checkout --ignore-submodules HEAD submodule &&
	git diff-files --quiet
'

test_expect_success '"checkout <submodule>" updates recursively' '
	git update-index --refresh &&
	git diff-files --quiet &&
	git diff-index --quiet --cached HEAD &&
	git checkout HEAD^ submodule &&
	git diff-files --quiet &&
	git checkout HEAD submodule &&
	git diff-files --quiet
'

test_expect_success '"checkout --ignore-submodules" updates the index only' '
	git checkout master &&
	git diff-files --quiet &&
	git diff-index --quiet --cached HEAD &&
	git checkout --ignore-submodules HEAD^ &&
	test_must_fail git diff-files --quiet &&
	git checkout --ignore-submodules master &&
	git diff-files --quiet
'

test_expect_success '"checkout" updates recursively' '
	git checkout master &&
	git diff-files --quiet &&
	git diff-index --quiet --cached HEAD &&
	git checkout HEAD^ &&
	git diff-files --quiet &&
	git checkout master &&
	git diff-files --quiet
'

test_expect_success '"checkout <submodule>" must use force when submodule contains modified content' '
	git checkout -f master &&
	git diff-files --quiet &&
	git diff-index --quiet --cached HEAD &&
	echo second >>submodule/first.t
	git checkout HEAD^ submodule &&
	test_must_fail git diff-files --quiet &&
	git checkout -f HEAD^ submodule &&
	git diff-files --quiet &&
	git checkout HEAD submodule &&
	git diff-files --quiet
'

test_expect_success '"checkout <submodule>" must use force when submodule contains untracked content' '
	git checkout -f master &&
	git diff-files --quiet &&
	git diff-index --quiet --cached HEAD &&
	echo second >>submodule/second.t
	git checkout HEAD^ submodule &&
	test_must_fail git diff-files --quiet &&
	git checkout -f HEAD^ submodule &&
	git diff-files --quiet &&
	git checkout HEAD submodule &&
	git diff-files --quiet
'

test_done
