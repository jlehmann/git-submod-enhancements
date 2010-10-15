#!/bin/sh

test_description='test rev-parse --cwd-to-worktree and --worktree-to-cwd'

. ./test-lib.sh

test_expect_success 'setup' '
	mkdir foo bar &&
	mv .git foo &&
	mkdir foo/bar &&
	GIT_DIR=`pwd`/foo/.git &&
	GIT_WORK_TREE=`pwd`/foo &&
	export GIT_DIR GIT_WORK_TREE
'

test_expect_success 'at root' '
	(
	cd foo &&
	git rev-parse --cwd-to-worktree --worktree-to-cwd >result &&
	: >expected &&
	test_cmp expected result
	)
'

test_expect_success 'cwd inside worktree' '
	(
	cd foo/bar &&
	git rev-parse --cwd-to-worktree --worktree-to-cwd >result &&
	echo ../ >expected &&
	echo bar/ >>expected &&
	test_cmp expected result
	)
'

test_expect_success 'cwd outside worktree' '
	git rev-parse --cwd-to-worktree --worktree-to-cwd >result &&
	echo foo/ >expected &&
	echo ../ >>expected &&
	test_cmp expected result
'

test_expect_success 'cwd outside worktree (2)' '
	(
	cd bar &&
	git rev-parse --cwd-to-worktree --worktree-to-cwd >result &&
	echo ../foo/ >expected &&
	echo ../bar/ >>expected &&
	test_cmp expected result
	)
'

test_done
