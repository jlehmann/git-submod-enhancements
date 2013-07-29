#!/bin/sh
#
# Copyright (c) 2011 Thomas Rast
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see http://www.gnu.org/licenses/ .

# do the --tee work early; it otherwise confuses our careful
# GIT_BUILD_DIR mangling
case "$GIT_TEST_TEE_STARTED, $* " in
done,*)
	# do not redirect again
	;;
*' --tee '*|*' --va'*)
	mkdir -p test-results
	BASE=test-results/$(basename "$0" .sh)
	(GIT_TEST_TEE_STARTED=done ${SHELL-sh} "$0" "$@" 2>&1;
	 echo $? > $BASE.exit) | tee $BASE.out
	test "$(cat $BASE.exit)" = 0
	exit
	;;
esac

TEST_DIRECTORY=$(pwd)/..
TEST_OUTPUT_DIRECTORY=$(pwd)
if test -z "$GIT_TEST_INSTALLED"; then
	perf_results_prefix=
else
	perf_results_prefix=$(printf "%s" "${GIT_TEST_INSTALLED%/bin-wrappers}" | tr -c "[a-zA-Z0-9]" "[_*]")"."
	# make the tested dir absolute
	GIT_TEST_INSTALLED=$(cd "$GIT_TEST_INSTALLED" && pwd)
fi

TEST_NO_CREATE_REPO=t
TEST_NO_MALLOC_CHECK=t

. ../test-lib.sh

# Variables from test-lib that are normally internal to the tests; we
# need to export them for test_perf subshells
export TEST_DIRECTORY TRASH_DIRECTORY GIT_BUILD_DIR GIT_TEST_CMP

perf_results_dir=$TEST_OUTPUT_DIRECTORY/test-results
mkdir -p "$perf_results_dir"
rm -f "$perf_results_dir"/$(basename "$0" .sh).subtests

if test -z "$GIT_PERF_REPEAT_COUNT"; then
	GIT_PERF_REPEAT_COUNT=3
fi
die_if_build_dir_not_repo () {
	if ! ( cd "$TEST_DIRECTORY/.." &&
		    git rev-parse --build-dir >/dev/null 2>&1 ); then
		error "No $1 defined, and your build directory is not a repo"
	fi
}

if test -z "$GIT_PERF_REPO"; then
	die_if_build_dir_not_repo '$GIT_PERF_REPO'
	GIT_PERF_REPO=$TEST_DIRECTORY/..
fi
if test -z "$GIT_PERF_LARGE_REPO"; then
	die_if_build_dir_not_repo '$GIT_PERF_LARGE_REPO'
	GIT_PERF_LARGE_REPO=$TEST_DIRECTORY/..
fi

test_perf_create_repo_from () {
	test "$#" = 2 ||
	error "bug in the test script: not 2 parameters to test-create-repo"
	repo="$1"
	source="$2"
	source_git=$source/$(cd "$source" && git rev-parse --git-dir)
	mkdir -p "$repo/.git"
	(
		cd "$repo/.git" &&
		{ cp -Rl "$source_git/objects" . 2>/dev/null ||
			cp -R "$source_git/objects" .; } &&
		for stuff in "$source_git"/*; do
			case "$stuff" in
				*/objects|*/hooks|*/config)
					;;
				*)
					cp -R "$stuff" . || break
					;;
			esac
		done &&
		cd .. &&
		git init -q &&
		mv .git/hooks .git/hooks-disabled 2>/dev/null
	) || error "failed to copy repository '$source' to '$repo'"
}

# call at least one of these to establish an appropriately-sized repository
test_perf_default_repo () {
	test_perf_create_repo_from "${1:-$TRASH_DIRECTORY}" "$GIT_PERF_REPO"
}
test_perf_large_repo () {
	if test "$GIT_PERF_LARGE_REPO" = "$GIT_BUILD_DIR"; then
		echo "warning: \$GIT_PERF_LARGE_REPO is \$GIT_BUILD_DIR." >&2
		echo "warning: This will work, but may not be a sufficiently large repo" >&2
		echo "warning: for representative measurements." >&2
	fi
	test_perf_create_repo_from "${1:-$TRASH_DIRECTORY}" "$GIT_PERF_LARGE_REPO"
}
test_checkout_worktree () {
	git checkout-index -u -a ||
	error "git checkout-index failed"
}

# Performance tests should never fail.  If they do, stop immediately
immediate=t

test_run_perf_ () {
	test_cleanup=:
	test_export_="test_cleanup"
	export test_cleanup test_export_
	/usr/bin/time -f "%E %U %S" -o test_time.$i "$SHELL" -c '
. '"$TEST_DIRECTORY"/test-lib-functions.sh'
test_export () {
	[ $# != 0 ] || return 0
	test_export_="$test_export_\\|$1"
	shift
	test_export "$@"
}
'"$1"'
ret=$?
set | sed -n "s'"/'/'\\\\''/g"';s/^\\($test_export_\\)/export '"'&'"'/p" >test_vars
exit $ret' >&3 2>&4
	eval_ret=$?

	if test $eval_ret = 0 || test -n "$expecting_failure"
	then
		test_eval_ "$test_cleanup"
		. ./test_vars || error "failed to load updated environment"
	fi
	if test "$verbose" = "t" && test -n "$HARNESS_ACTIVE"; then
		echo ""
	fi
	return "$eval_ret"
}


test_perf () {
	test_start_
	test "$#" = 3 && { test_prereq=$1; shift; } || test_prereq=
	test "$#" = 2 ||
	error "bug in the test script: not 2 or 3 parameters to test-expect-success"
	export test_prereq
	if ! test_skip "$@"
	then
		base=$(basename "$0" .sh)
		echo "$test_count" >>"$perf_results_dir"/$base.subtests
		echo "$1" >"$perf_results_dir"/$base.$test_count.descr
		if test -z "$verbose"; then
			printf "%s" "perf $test_count - $1:"
		else
			echo "perf $test_count - $1:"
		fi
		for i in $(test_seq 1 $GIT_PERF_REPEAT_COUNT); do
			say >&3 "running: $2"
			if test_run_perf_ "$2"
			then
				if test -z "$verbose"; then
					printf " %s" "$i"
				else
					echo "* timing run $i/$GIT_PERF_REPEAT_COUNT:"
				fi
			else
				test -z "$verbose" && echo
				test_failure_ "$@"
				break
			fi
		done
		if test -z "$verbose"; then
			echo " ok"
		else
			test_ok_ "$1"
		fi
		base="$perf_results_dir"/"$perf_results_prefix$(basename "$0" .sh)"."$test_count"
		"$TEST_DIRECTORY"/perf/min_time.perl test_time.* >"$base".times
	fi
	test_finish_
}

# We extend test_done to print timings at the end (./run disables this
# and does it after running everything)
test_at_end_hook_ () {
	if test -z "$GIT_PERF_AGGREGATING_LATER"; then
		( cd "$TEST_DIRECTORY"/perf && ./aggregate.perl $(basename "$0") )
	fi
}

test_export () {
	export "$@"
}
