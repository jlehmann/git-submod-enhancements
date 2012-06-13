#!/bin/sh

test_description='checkout can handle submodules'

. ./test-lib.sh

submodule_creation_must_succeed() {
	# checkout base ($1)
	git checkout -f --recurse-submodules $1 &&
	git diff-files --quiet &&
	git diff-index --quiet --cached $1 &&

	# checkout target ($2)
	if test -d submodule; then
		echo change>>submodule/first.t &&
		test_must_fail git checkout --recurse-submodules $2 &&
		git checkout -f --recurse-submodules $2
	else
		git checkout --recurse-submodules $2
	fi &&
	test -e submodule/.git &&
	test -f submodule/first.t &&
	test -f submodule/second.t &&
	git diff-files --quiet &&
	git diff-index --quiet --cached $2
}

submodule_removal_must_succeed() {
	# checkout base ($1)
	git checkout -f --recurse-submodules $1 &&
	git submodule update -f &&
	test -e submodule/.git &&
	git diff-files --quiet &&
	git diff-index --quiet --cached $1 &&

	# checkout target ($2)
	echo change>>submodule/first.t &&
	test_must_fail git checkout --recurse-submodules $2 &&
	git checkout -f --recurse-submodules $2 &&
	git diff-files --quiet &&
	git diff-index --quiet --cached $2 &&
	! test -d submodule
}

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
	git reset HEAD^ submodule &&
	test_must_fail git diff-files --quiet &&
	git reset submodule &&
	git diff-files --quiet
'

test_expect_success '"checkout <submodule>" updates the index only' '
	git update-index --refresh &&
	git diff-files --quiet &&
	git diff-index --quiet --cached HEAD &&
	git checkout HEAD^ submodule &&
	test_must_fail git diff-files --quiet &&
	git checkout HEAD submodule &&
	git diff-files --quiet &&
	git diff-index --quiet --cached HEAD
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

test_expect_success '"checkout --recurse-submodules" removes deleted submodule' '
	git config -f .gitmodules submodule.submodule.path submodule &&
	git config -f .gitmodules submodule.submodule.url submodule.bare &&
	(cd submodule && git clone --bare . ../submodule.bare) &&
	echo submodule.bare >>.gitignore &&
	git config submodule.submodule.ignore none &&
	git add .gitignore .gitmodules submodule &&
	git submodule update --init &&
	git commit -m "submodule registered" &&
	git checkout -b base &&
	git checkout -b delete_submodule &&
	rm -rf submodule &&
	git rm submodule &&
	git commit -m "submodule deleted" &&
	submodule_removal_must_succeed base delete_submodule
'

test_expect_success '"checkout --recurse-submodules" repopulates submodule' '
	submodule_creation_must_succeed delete_submodule base
'

test_expect_success '"checkout --recurse-submodules" repopulates submodule in existing directory' '
	git checkout --recurse-submodules delete_submodule &&
	mkdir submodule &&
	submodule_creation_must_succeed delete_submodule base
'

test_expect_success '"checkout --recurse-submodules" replaces submodule with files' '
	git checkout -f base &&
	git checkout -b replace_submodule_with_dir &&
	git update-index --force-remove submodule &&
	rm -rf submodule/.git .gitmodules &&
	git add .gitmodules submodule/* &&
	git commit -m "submodule replaced" &&
	git checkout -f base &&
	git submodule update -f &&
	git checkout --recurse-submodules replace_submodule_with_dir &&
	test -d submodule &&
	! test -e submodule/.git &&
	test -f submodule/first.t &&
	test -f submodule/second.t
'

test_expect_success '"checkout --recurse-submodules" removes files and repopulates submodule' '
	submodule_creation_must_succeed replace_submodule_with_dir base
'

test_expect_failure '"checkout --recurse-submodules" replaces submodule with a file' '
	git checkout -f base &&
	git checkout -b replace_submodule_with_file &&
	git update-index --force-remove submodule &&
	rm -rf submodule .gitmodules &&
	echo content >submodule &&
	git add .gitmodules submodule &&
	git commit -m "submodule replaced with file" &&
	git checkout -f base &&
	git submodule update -f &&
	git checkout --recurse-submodules replace_submodule_with_file &&
	test -d submodule &&
	! test -e submodule/.git &&
	test -f submodule/first.t &&
	test -f submodule/second.t
'

test_expect_success '"checkout --recurse-submodules" removes the file and repopulates submodule' '
	submodule_creation_must_succeed replace_submodule_with_file base
'

test_expect_failure '"checkout --recurse-submodules" replaces submodule with a link' '
	git checkout -f base &&
	git checkout -b replace_submodule_with_link &&
	git update-index --force-remove submodule &&
	rm -rf submodule .gitmodules &&
	ln -s submodule &&
	git add .gitmodules submodule &&
	git commit -m "submodule replaced with link" &&
	git checkout -f base &&
	git submodule update -f &&
	git checkout --recurse-submodules replace_submodule_with_link &&
	test -d submodule &&
	! test -e submodule/.git &&
	test -f submodule/first.t &&
	test -f submodule/second.t
'

test_expect_success '"checkout --recurse-submodules" removes the link and repopulates submodule' '
	submodule_creation_must_succeed replace_submodule_with_link base
'

test_expect_success '"checkout --recurse-submodules" updates recursively' '
	git checkout --recurse-submodules base &&
	git diff-files --quiet &&
	git diff-index --quiet --cached HEAD &&
	git checkout -b updated_submodule &&
	(cd submodule &&
	 echo x >>first.t &&
	 git add first.t &&
	 test_commit third) &&
	git add submodule &&
	test_tick &&
	git commit -m updated.superproject &&
	git checkout --recurse-submodules base &&
	git diff-files --quiet &&
	git diff-index --quiet --cached HEAD
'

test_expect_failure '"checkout --recurse-submodules" needs -f to update a modifed submodule commit' '
	(
		cd submodule &&
		git checkout --recurse-submodules HEAD^
	) &&
	test_must_fail git checkout --recurse-submodules master &&
	test_must_fail git diff-files --quiet submodule &&
	git diff-files --quiet file &&
	git checkout --recurse-submodules -f master &&
	git diff-files --quiet &&
	git diff-index --quiet --cached HEAD
'

test_expect_failure '"checkout --recurse-submodules" needs -f to update modifed submodule content' '
	echo modified >submodule/second.t &&
	test_must_fail git checkout --recurse-submodules HEAD^ &&
	test_must_fail git diff-files --quiet submodule &&
	git diff-files --quiet file &&
	git checkout --recurse-submodules -f HEAD^ &&
	git diff-files --quiet &&
	git diff-index --quiet --cached HEAD &&
	git checkout --recurse-submodules -f master &&
	git diff-files --quiet &&
	git diff-index --quiet --cached HEAD
'

test_expect_failure '"checkout --recurse-submodules" ignores modified submodule content that would not be changed' '
	echo modified >expected &&
	cp expected submodule/first.t &&
	git checkout --recurse-submodules HEAD^ &&
	test_cmp expected submodule/first.t &&
	test_must_fail git diff-files --quiet submodule &&
	git diff-index --quiet --cached HEAD &&
	git checkout --recurse-submodules -f master &&
	git diff-files --quiet &&
	git diff-index --quiet --cached HEAD
'

test_expect_failure '"checkout --recurse-submodules" does not care about untracked submodule content' '
	echo untracked >submodule/untracked &&
	git checkout --recurse-submodules master &&
	git diff-files --quiet --ignore-submodules=untracked &&
	git diff-index --quiet --cached HEAD &&
	rm submodule/untracked
'

test_expect_failure '"checkout --recurse-submodules" needs -f when submodule commit is not present (but does fail anyway)' '
	git checkout --recurse-submodules -b bogus_commit master &&
	git update-index --cacheinfo 160000 0123456789012345678901234567890123456789 submodule
	BOGUS_TREE=$(git write-tree) &&
	BOGUS_COMMIT=$(echo "bogus submodule commit" | git commit-tree $BOGUS_TREE) &&
	git commit -m "bogus submodule commit" &&
	git checkout --recurse-submodules -f master &&
	test_must_fail git checkout --recurse-submodules bogus_commit &&
	git diff-files --quiet &&
	test_must_fail git checkout --recurse-submodules -f bogus_commit &&
	test_must_fail git diff-files --quiet submodule &&
	git diff-files --quiet file &&
	git diff-index --quiet --cached HEAD &&
	git checkout --recurse-submodules -f master
'

test_done
