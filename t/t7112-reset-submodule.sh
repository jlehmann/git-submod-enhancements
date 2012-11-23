#!/bin/sh
#
# Copyright (c) 2012 Heiko Voigt
#

test_description='submodule support for reset

This test tries to verify that reset handles submodules correctly.
'

. ./test-lib.sh

test_expect_success 'setup' '
	git config receive.denyCurrentBranch ignore &&
	git clone . submodule &&
	(cd submodule &&
		git remote rm origin &&
	 	touch suba &&
	 	git add suba &&
	 	git commit -msuba
	 	git checkout -b sub_branch_b &&
	 	touch subb &&
	 	git add subb &&
	 	git commit -msubb &&
	 	git checkout master
	) &&
	git clone . super &&
	(cd super &&
	 	touch a &&
	 	git add a &&
	 	git commit -ma &&
	 	git submodule add ../submodule &&
	 	git commit -m "add submodule" &&
		git push origin master
	)
'

test_expect_success 'working directory is clean after reset' '
	(cd super &&
		(cd submodule && git rev-parse HEAD) >rev_expected &&
		git diff >diff_expected &&
		(cd submodule &&
			git checkout sub_branch_b
		) &&
		git reset --hard &&
		(cd submodule && git rev-parse HEAD) >rev_actual &&
		test_cmp rev_expected rev_actual &&
		git diff >diff_actual &&
		test_cmp diff_expected diff_actual
	)
'

test_expect_success 'working directory is not clean with --no-recurse-submodules' '
	(cd super &&
		(cd submodule &&
			git checkout sub_branch_b
		) &&
		(cd submodule && git rev-parse HEAD) >rev_expected &&
		git diff >diff_expected &&
		git reset --hard --no-recurse-submodules &&
		(cd submodule && git rev-parse HEAD) >rev_actual &&
		test_cmp rev_expected rev_actual &&
		git diff >diff_actual &&
		test_cmp diff_expected diff_actual &&
		(cd submodule &&
			git checkout master
		)
	)
'

test_expect_failure 'reset --[no-]recurse-submodules needs --hard' '
	(cd super &&
		test_must_fail git reset --no-recurse-submodules &&
		test_must_fail git reset --recurse-submodules
	)
'

test_expect_success 'reset fails with non-fetched commit in submodule' '
	(cd submodule &&
		touch subc &&
		git add subc &&
		git commit -msubc
	)
	git clone . super2 &&
	(cd super2 &&
		git submodule update --init &&
		(cd submodule &&
			git checkout origin/master
		) &&
		git add submodule &&
		git commit -msubc
		git push origin master
	) &&
	(cd super &&
		git fetch --no-recurse-submodules &&
		test_must_fail git reset --hard origin/master
	)
'

test_expect_failure 'reset --no-recurse-submodule does not touch dirty submodule' '
	(cd super &&
		(cd submodule &&
			echo dirty >suba
		) &&
		cp submodule/suba suba_expected &&
		git diff >diff_expected &&
		(cd submodule && git rev-parse HEAD) >rev_expected &&
		git reset --hard --no-recurse-submodule &&
		test_cmp suba_expected submodule/suba &&
		git diff >diff_actual &&
		test_cmp diff_expected diff_actual &&
		(cd submodule && git rev-parse HEAD) >rev_actual &&
		test_cmp rev_expected rev_actual &&
		(cd submodule &&
			git checkout suba
		)
	)
'

test_expect_failure 'reset --hard works on dirty submodule' '
	(cd super &&
		git diff >diff_expected &&
		(cd submodule && git rev-parse HEAD) >rev_expected &&
		(cd submodule &&
			echo dirty >suba
		) &&
		git reset --hard &&
		git diff >diff_actual &&
		test_cmp diff_expected diff_actual &&
		(cd submodule && git rev-parse HEAD) >rev_actual &&
		test_cmp rev_expected rev_actual
	)
'

test_done
