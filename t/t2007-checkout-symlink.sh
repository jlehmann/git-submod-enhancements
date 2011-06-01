#!/bin/sh
#
# Copyright (c) 2007 Junio C Hamano

test_description='git checkout to switch between branches with symlink<->dir'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-checkout.sh

test_expect_success SYMLINKS setup '

	mkdir frotz &&
	echo hello >frotz/filfre &&
	git add frotz/filfre &&
	test_tick &&
	git commit -m "master has file frotz/filfre" &&

	git branch side &&

	echo goodbye >nitfol &&
	git add nitfol &&
	test_tick &&
	git commit -m "master adds file nitfol" &&

	git checkout side &&

	git rm --cached frotz/filfre &&
	mv frotz xyzzy &&
	ln -s xyzzy frotz &&
	git add xyzzy/filfre frotz &&
	test_tick &&
	git commit -m "side moves frotz/ to xyzzy/ and adds frotz->xyzzy/"

'

test_expect_success SYMLINKS 'switch from symlink to dir' '

	checkout_must_succeed master

'

test_expect_success SYMLINKS 'Remove temporary directories & switch to master' '
	rm -fr frotz xyzzy nitfol &&
	checkout_must_succeed -f master
'

test_expect_success SYMLINKS 'switch from dir to symlink' '

	checkout_must_succeed side

'

test_done
