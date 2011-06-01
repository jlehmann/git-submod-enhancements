#!/bin/sh

test_description='checkout must not overwrite an untracked objects'
. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-checkout.sh

test_expect_success 'setup' '

	mkdir -p a/b/c &&
	>a/b/c/d &&
	git add -A &&
	git commit -m base &&
	git tag start
'

test_expect_success 'create a commit where dir a/b changed to file' '

	checkout_must_succeed -b file &&
	rm -rf a/b &&
	>a/b &&
	git add -A &&
	git commit -m "dir to file"
'

test_expect_success 'checkout commit with dir must not remove untracked a/b' '

	git rm --cached a/b &&
	git commit -m "un-track the file" &&
	checkout_must_fail start &&
	test -f a/b
'

test_expect_success SYMLINKS 'create a commit where dir a/b changed to symlink' '

	rm -rf a/b &&	# cleanup if previous test failed
	checkout_must_succeed -f -b symlink start &&
	rm -rf a/b &&
	ln -s foo a/b &&
	git add -A &&
	git commit -m "dir to symlink"
'

test_expect_success SYMLINKS 'checkout commit with dir must not remove untracked a/b' '

	git rm --cached a/b &&
	git commit -m "un-track the symlink" &&
	checkout_must_fail start &&
	test -h a/b
'

test_done
