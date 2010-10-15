#!/bin/sh
#
# Copyright (c) 2010 Chris Packham
#

test_description='git grep --recursive test

This test checks the ability of git grep to search within submodules when told
to do so with the --recursive option'

. ./test-lib.sh

restore_test_defaults()
{
	unset GIT_SUPER_REFNAME
}

test_expect_success 'setup' '
	cat >t <<-\EOF &&
	one two three
	four five six
	seven eight nine
	EOF
	git add t &&
	git commit -m "initial commit"
'
submodurl=$TRASH_DIRECTORY

test_expect_success 'setup submodules' '
	for mod in submodule1 submodule2 submodule3 submodule4 submodule5; do
		git submodule add "$submodurl" $mod &&
		git submodule init $mod
	done &&
	git commit -m "setup submodules for test"
'

test_expect_success 'update data in each submodule' '
	for n in 1 2 3 4 5; do
		(cd submodule$n &&
			sed -i "s/^four.*/& #$n/" t &&
			git commit -a -m"update") &&
		git add submodule$n
	done &&
	git commit -m "update data in each submodule"
'

test_expect_success 'non-recursive grep in base' '
	cat >expected <<-\EOF &&
	t:four five six
	EOF
	git grep "five" >actual &&
	test_cmp expected actual
'

test_expect_success 'submodule-ref option' '
	cat >expected <<-\EOF &&
	bar:t:four five six
	EOF
	GIT_SUPER_REFNAME=bar &&
	export GIT_SUPER_REFNAME &&
	git grep "five" master >actual &&
	test_cmp expected actual &&
	restore_test_defaults
'

test_expect_success 'non-recursive grep in submodule' '
	(
		cd submodule1 &&
		cat >expected <<-\EOF &&
		t:four five six #1
		EOF
		git grep "five" >actual &&
		test_cmp expected actual
	)
'

test_expect_success 'recursive grep' '
	cat >expected <<-\EOF &&
	submodule1/t:four five six #1
	submodule2/t:four five six #2
	submodule3/t:four five six #3
	submodule4/t:four five six #4
	submodule5/t:four five six #5
	t:four five six
	EOF
	git grep --recursive "five" >actual &&
	test_cmp expected actual
'

test_expect_success 'recursive grep (with -n)' '
	cat >expected <<-\EOF &&
	submodule1/t:2:four five six #1
	submodule2/t:2:four five six #2
	submodule3/t:2:four five six #3
	submodule4/t:2:four five six #4
	submodule5/t:2:four five six #5
	t:2:four five six
	EOF
	git grep --recursive -n "five" >actual &&
	test_cmp expected actual
'

test_expect_success 'recursive grep (with -l)' '
	cat >expected <<-\EOF &&
	submodule1/t
	submodule2/t
	submodule3/t
	submodule4/t
	submodule5/t
	t
	EOF
	git grep --recursive -l "five" >actual &&
	test_cmp expected actual
'

test_expect_success 'recursive grep (with --or)' '
	cat >expected <<-\EOF &&
	submodule1/t:one two three
	submodule1/t:four five six #1
	submodule2/t:one two three
	submodule2/t:four five six #2
	submodule3/t:one two three
	submodule3/t:four five six #3
	submodule4/t:one two three
	submodule4/t:four five six #4
	submodule5/t:one two three
	submodule5/t:four five six #5
	t:one two three
	t:four five six
	EOF
	git grep --recursive \( -e "five" --or -e "two" \) >actual &&
	test_cmp expected actual
'

test_expect_success 'recursive grep (with --and --not)' '
	cat >expected <<-\EOF &&
	submodule2/t:four five six #2
	submodule3/t:four five six #3
	submodule4/t:four five six #4
	submodule5/t:four five six #5
	t:four five six
	EOF
	git grep --recursive \( -e "five" --and --not -e "#1" \) >actual &&
	test_cmp expected actual
'

test_expect_success 'recursive grep with refspec' '
	cat >expected <<-\EOF &&
	master:submodule1/t:four five six #1
	master:submodule2/t:four five six #2
	master:submodule3/t:four five six #3
	master:submodule4/t:four five six #4
	master:submodule5/t:four five six #5
	master:t:four five six
	EOF
	git grep --recursive five master >actual &&
	test_cmp expected actual
'

test_expect_success 'recursive grep with pathspec' '
	cat >expected <<-\EOF &&
	submodule2/t:four five six #2
	EOF
	git grep --recursive five -- submodule2 >actual &&
	test_cmp expected actual
'

test_expect_success 'recursive grep with pathspec and refspec' '
	cat >expected <<-\EOF &&
	master:submodule2/t:four five six #2
	EOF
	git grep --recursive five master -- submodule2 >actual &&
	test_cmp expected actual
'

test_expect_failure 'recursive grep with --max-depth' '
	cat >expected <<-\EOF &&
	t:four five six
	EOF
	git grep --recursive --max-depth=1 five  >actual &&
	test_cmp expected actual
'
test_done
