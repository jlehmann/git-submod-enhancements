#!/bin/sh

test_description='checkout can handle submodules'

. ./test-lib.sh

test_expect_success 'setup' '
	mkdir submodule &&
	(cd submodule &&
	 git init &&
	 test_commit first) &&
	echo first > file &&
	git add file submodule &&
	test_tick &&
	git commit -m superproject &&
	(cd submodule &&
	 test_commit second) &&
	echo second > file &&
	git add file submodule &&
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

test_expect_success '"checkout --no-recurse-submodules <submodule>" updates the index only' '
	git update-index --refresh &&
	git diff-files --quiet &&
	git diff-index --quiet --cached HEAD &&
	git checkout --no-recurse-submodules HEAD^ submodule &&
	test_must_fail git diff-files --quiet submodule &&
	git checkout HEAD submodule &&
	git diff-files --quiet &&
	git diff-index --quiet --cached HEAD
'

test_expect_success '"checkout" updates recursively' '
	git update-index --refresh &&
	git diff-files --quiet &&
	git diff-index --quiet --cached HEAD &&
	git checkout HEAD^ &&
	git diff-files --quiet &&
	git diff-index --quiet --cached HEAD
'

test_expect_success '"checkout" needs -f to update a modifed submodule commit' '
	(
		cd submodule &&
		git checkout master
	) &&
	test_must_fail git checkout master &&
	test_must_fail git diff-files --quiet submodule &&
	git diff-files --quiet file &&
	git checkout -f master &&
	git diff-files --quiet &&
	git diff-index --quiet --cached HEAD
'

test_expect_success '"checkout" needs -f to update modifed submodule content' '
	echo modified >submodule/second.t &&
	test_must_fail git checkout HEAD^ &&
	test_must_fail git diff-files --quiet submodule &&
	git diff-files --quiet file &&
	git checkout -f HEAD^ &&
	git diff-files --quiet &&
	git diff-index --quiet --cached HEAD &&
	git checkout -f master &&
	git diff-files --quiet &&
	git diff-index --quiet --cached HEAD
'

test_expect_success '"checkout" ignores modified submodule content that would not be changed' '
	echo modified >expected &&
	cp expected submodule/first.t &&
	git checkout HEAD^ &&
	test_cmp expected submodule/first.t
	test_must_fail git diff-files --quiet submodule &&
	git diff-index --quiet --cached HEAD &&
	git checkout -f master &&
	git diff-files --quiet &&
	git diff-index --quiet --cached HEAD
'

test_expect_success '"checkout" does not care about untracked submodule content' '
	echo untracked >submodule/untracked &&
	git checkout master &&
	git diff-files --quiet --ignore-submodules=untracked &&
	git diff-index --quiet --cached HEAD &&
	rm submodule/untracked
'

test_expect_success '"checkout" needs -f when submodule commit is not present (but does fail anyway)' '
	git checkout -b bogus_commit master &&
	git update-index --cacheinfo 160000 0123456789012345678901234567890123456789 submodule
	BOGUS_TREE=$(git write-tree) &&
	BOGUS_COMMIT=$(echo "bogus submodule commit" | git commit-tree $BOGUS_TREE) &&
	git commit -m "bogus submodule commit" &&
	git checkout -f master &&
	test_must_fail git checkout bogus_commit &&
	git diff-files --quiet &&
	test_must_fail git checkout -f bogus_commit &&
	test_must_fail git diff-files --quiet submodule &&
	git diff-files --quiet file &&
	git diff-index --quiet --cached HEAD &&
	git checkout -f master
'

test_expect_success '"checkout <submodule>" honors diff.ignoreSubmodules' '
	git config diff.ignoreSubmodules dirty &&
	echo x> submodule/untracked &&
	git checkout HEAD >actual 2>&1 &&
	! test -s actual
'

test_expect_success '"checkout <submodule>" honors submodule.*.ignore from .gitmodules' '
	git config diff.ignoreSubmodules none &&
	git config -f .gitmodules submodule.submodule.path submodule &&
	git config -f .gitmodules submodule.submodule.ignore untracked &&
	git checkout HEAD >actual 2>&1 &&
	! test -s actual
'

test_expect_success '"checkout <submodule>" honors submodule.*.ignore from .git/config' '
	git config -f .gitmodules submodule.submodule.ignore none &&
	git config submodule.submodule.path submodule &&
	git config submodule.submodule.ignore all &&
	git checkout HEAD >actual 2>&1 &&
	! test -s actual
'

test_done
