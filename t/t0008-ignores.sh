#!/bin/sh

test_description=check-ignore

. ./test-lib.sh

init_vars () {
	global_excludes="$(pwd)/global-excludes"
}

enable_global_excludes () {
	init_vars &&
	git config core.excludesfile "$global_excludes"
}

expect_in () {
	dest="$HOME/expected-$1" text="$2"
	if test -z "$text"
	then
		>"$dest" # avoid newline
	else
		echo "$text" >"$dest"
	fi
}

expect () {
	expect_in stdout "$1"
}

expect_from_stdin () {
	cat >"$HOME/expected-stdout"
}

test_stderr () {
	expected="$1"
	expect_in stderr "$1" &&
	test_cmp "$HOME/expected-stderr" "$HOME/stderr"
}

stderr_contains () {
	regexp="$1"
	if grep "$regexp" "$HOME/stderr"
	then
		return 0
	else
		echo "didn't find /$regexp/ in $HOME/stderr"
		cat "$HOME/stderr"
		return 1
	fi
}

stderr_empty_on_success () {
	expect_code="$1"
	if test $expect_code = 0
	then
		test_stderr ""
	else
		# If we expect failure then stderr might or might not be empty
		# due to --quiet - the caller can check its contents
		return 0
	fi
}

test_check_ignore () {
	args="$1" expect_code="${2:-0}" global_args="$3"

	init_vars &&
	rm -f "$HOME/stdout" "$HOME/stderr" "$HOME/cmd" &&
	echo git $global_args check-ignore $quiet_opt $verbose_opt $non_matching_opt $args \
		>"$HOME/cmd" &&
	echo "$expect_code" >"$HOME/expected-exit-code" &&
	test_expect_code "$expect_code" \
		git $global_args check-ignore $quiet_opt $verbose_opt $non_matching_opt $args \
		>"$HOME/stdout" 2>"$HOME/stderr" &&
	test_cmp "$HOME/expected-stdout" "$HOME/stdout" &&
	stderr_empty_on_success "$expect_code"
}

# Runs the same code with 4 different levels of output verbosity:
#
#   1. with -q / --quiet
#   2. with default verbosity
#   3. with -v / --verbose
#   4. with -v / --verbose, *and* -n / --non-matching
#
# expecting success each time.  Takes advantage of the fact that
# check-ignore --verbose output is the same as normal output except
# for the extra first column.
#
# Arguments:
#   - (optional) prereqs for this test, e.g. 'SYMLINKS'
#   - test name
#   - output to expect from the fourth verbosity mode (the output
#     from the other verbosity modes is automatically inferred
#     from this value)
#   - code to run (should invoke test_check_ignore)
test_expect_success_multi () {
	prereq=
	if test $# -eq 4
	then
		prereq=$1
		shift
	fi
	testname="$1" expect_all="$2" code="$3"

	expect_verbose=$( echo "$expect_all" | grep -v '^::	' )
	expect=$( echo "$expect_verbose" | sed -e 's/.*	//' )

	test_expect_success $prereq "$testname" '
		expect "$expect" &&
		eval "$code"
	'

	# --quiet is only valid when a single pattern is passed
	if test $( echo "$expect_all" | wc -l ) = 1
	then
		for quiet_opt in '-q' '--quiet'
		do
			test_expect_success $prereq "$testname${quiet_opt:+ with $quiet_opt}" "
			expect '' &&
			$code
		"
		done
		quiet_opt=
	fi

	for verbose_opt in '-v' '--verbose'
	do
		for non_matching_opt in '' ' -n' ' --non-matching'
		do
			if test -n "$non_matching_opt"
			then
				my_expect="$expect_all"
			else
				my_expect="$expect_verbose"
			fi

			test_code="
				expect '$my_expect' &&
				$code
			"
			opts="$verbose_opt$non_matching_opt"
			test_expect_success $prereq "$testname${opts:+ with $opts}" "$test_code"
		done
	done
	verbose_opt=
	non_matching_opt=
}

test_expect_success 'setup' '
	init_vars &&
	mkdir -p a/b/ignored-dir a/submodule b &&
	if test_have_prereq SYMLINKS
	then
		ln -s b a/symlink
	fi &&
	(
		cd a/submodule &&
		git init &&
		echo a >a &&
		git add a &&
		git commit -m"commit in submodule"
	) &&
	git add a/submodule &&
	cat <<-\EOF >.gitignore &&
		one
		ignored-*
		top-level-dir/
	EOF
	for dir in . a
	do
		: >$dir/not-ignored &&
		: >$dir/ignored-and-untracked &&
		: >$dir/ignored-but-in-index
	done &&
	git add -f ignored-but-in-index a/ignored-but-in-index &&
	cat <<-\EOF >a/.gitignore &&
		two*
		*three
	EOF
	cat <<-\EOF >a/b/.gitignore &&
		four
		five
		# this comment should affect the line numbers
		six
		ignored-dir/
		# and so should this blank line:

		!on*
		!two
	EOF
	echo "seven" >a/b/ignored-dir/.gitignore &&
	test -n "$HOME" &&
	cat <<-\EOF >"$global_excludes" &&
		globalone
		!globaltwo
		globalthree
	EOF
	cat <<-\EOF >>.git/info/exclude
		per-repo
	EOF
'

############################################################################
#
# test invalid inputs

test_expect_success_multi '. corner-case' '::	.' '
	test_check_ignore . 1
'

test_expect_success_multi 'empty command line' '' '
	test_check_ignore "" 128 &&
	stderr_contains "fatal: no path specified"
'

test_expect_success_multi '--stdin with empty STDIN' '' '
	test_check_ignore "--stdin" 1 </dev/null &&
	test_stderr ""
'

test_expect_success '-q with multiple args' '
	expect "" &&
	test_check_ignore "-q one two" 128 &&
	stderr_contains "fatal: --quiet is only valid with a single pathname"
'

test_expect_success '--quiet with multiple args' '
	expect "" &&
	test_check_ignore "--quiet one two" 128 &&
	stderr_contains "fatal: --quiet is only valid with a single pathname"
'

for verbose_opt in '-v' '--verbose'
do
	for quiet_opt in '-q' '--quiet'
	do
		test_expect_success "$quiet_opt $verbose_opt" "
			expect '' &&
			test_check_ignore '$quiet_opt $verbose_opt foo' 128 &&
			stderr_contains 'fatal: cannot have both --quiet and --verbose'
		"
	done
done

test_expect_success '--quiet with multiple args' '
	expect "" &&
	test_check_ignore "--quiet one two" 128 &&
	stderr_contains "fatal: --quiet is only valid with a single pathname"
'

test_expect_success_multi 'erroneous use of --' '' '
	test_check_ignore "--" 128 &&
	stderr_contains "fatal: no path specified"
'

test_expect_success_multi '--stdin with superfluous arg' '' '
	test_check_ignore "--stdin foo" 128 &&
	stderr_contains "fatal: cannot specify pathnames with --stdin"
'

test_expect_success_multi '--stdin -z with superfluous arg' '' '
	test_check_ignore "--stdin -z foo" 128 &&
	stderr_contains "fatal: cannot specify pathnames with --stdin"
'

test_expect_success_multi '-z without --stdin' '' '
	test_check_ignore "-z" 128 &&
	stderr_contains "fatal: -z only makes sense with --stdin"
'

test_expect_success_multi '-z without --stdin and superfluous arg' '' '
	test_check_ignore "-z foo" 128 &&
	stderr_contains "fatal: -z only makes sense with --stdin"
'

test_expect_success_multi 'needs work tree' '' '
	(
		cd .git &&
		test_check_ignore "foo" 128
	) &&
	stderr_contains "fatal: This operation must be run in a work tree"
'

############################################################################
#
# test standard ignores

# First make sure that the presence of a file in the working tree
# does not impact results, but that the presence of a file in the
# index does.

for subdir in '' 'a/'
do
	if test -z "$subdir"
	then
		where="at top-level"
	else
		where="in subdir $subdir"
	fi

	test_expect_success_multi "non-existent file $where not ignored" \
		"::	${subdir}non-existent" \
		"test_check_ignore '${subdir}non-existent' 1"

	test_expect_success_multi "non-existent file $where ignored" \
		".gitignore:1:one	${subdir}one" \
		"test_check_ignore '${subdir}one'"

	test_expect_success_multi "existing untracked file $where not ignored" \
		"::	${subdir}not-ignored" \
		"test_check_ignore '${subdir}not-ignored' 1"

	test_expect_success_multi "existing tracked file $where not ignored" \
		"::	${subdir}ignored-but-in-index" \
		"test_check_ignore '${subdir}ignored-but-in-index' 1"

	test_expect_success_multi "existing untracked file $where ignored" \
		".gitignore:2:ignored-*	${subdir}ignored-and-untracked" \
		"test_check_ignore '${subdir}ignored-and-untracked'"

	test_expect_success_multi "mix of file types $where" \
"::	${subdir}non-existent
.gitignore:1:one	${subdir}one
::	${subdir}not-ignored
::	${subdir}ignored-but-in-index
.gitignore:2:ignored-*	${subdir}ignored-and-untracked" \
		"test_check_ignore '
			${subdir}non-existent
			${subdir}one
			${subdir}not-ignored
			${subdir}ignored-but-in-index
			${subdir}ignored-and-untracked'
		"
done

# Having established the above, from now on we mostly test against
# files which do not exist in the working tree or index.

test_expect_success 'sub-directory local ignore' '
	expect "a/3-three" &&
	test_check_ignore "a/3-three a/three-not-this-one"
'

test_expect_success 'sub-directory local ignore with --verbose'  '
	expect "a/.gitignore:2:*three	a/3-three" &&
	test_check_ignore "--verbose a/3-three a/three-not-this-one"
'

test_expect_success 'local ignore inside a sub-directory' '
	expect "3-three" &&
	(
		cd a &&
		test_check_ignore "3-three three-not-this-one"
	)
'
test_expect_success 'local ignore inside a sub-directory with --verbose' '
	expect "a/.gitignore:2:*three	3-three" &&
	(
		cd a &&
		test_check_ignore "--verbose 3-three three-not-this-one"
	)
'

test_expect_success_multi 'nested include' \
	'a/b/.gitignore:8:!on*	a/b/one' '
	test_check_ignore "a/b/one"
'

############################################################################
#
# test ignored sub-directories

test_expect_success_multi 'ignored sub-directory' \
	'a/b/.gitignore:5:ignored-dir/	a/b/ignored-dir' '
	test_check_ignore "a/b/ignored-dir"
'

test_expect_success 'multiple files inside ignored sub-directory' '
	expect_from_stdin <<-\EOF &&
		a/b/ignored-dir/foo
		a/b/ignored-dir/twoooo
		a/b/ignored-dir/seven
	EOF
	test_check_ignore "a/b/ignored-dir/foo a/b/ignored-dir/twoooo a/b/ignored-dir/seven"
'

test_expect_success 'multiple files inside ignored sub-directory with -v' '
	expect_from_stdin <<-\EOF &&
		a/b/.gitignore:5:ignored-dir/	a/b/ignored-dir/foo
		a/b/.gitignore:5:ignored-dir/	a/b/ignored-dir/twoooo
		a/b/.gitignore:5:ignored-dir/	a/b/ignored-dir/seven
	EOF
	test_check_ignore "-v a/b/ignored-dir/foo a/b/ignored-dir/twoooo a/b/ignored-dir/seven"
'

test_expect_success 'cd to ignored sub-directory' '
	expect_from_stdin <<-\EOF &&
		foo
		twoooo
		../one
		seven
		../../one
	EOF
	(
		cd a/b/ignored-dir &&
		test_check_ignore "foo twoooo ../one seven ../../one"
	)
'

test_expect_success 'cd to ignored sub-directory with -v' '
	expect_from_stdin <<-\EOF &&
		a/b/.gitignore:5:ignored-dir/	foo
		a/b/.gitignore:5:ignored-dir/	twoooo
		a/b/.gitignore:8:!on*	../one
		a/b/.gitignore:5:ignored-dir/	seven
		.gitignore:1:one	../../one
	EOF
	(
		cd a/b/ignored-dir &&
		test_check_ignore "-v foo twoooo ../one seven ../../one"
	)
'

############################################################################
#
# test handling of symlinks

test_expect_success_multi SYMLINKS 'symlink' '::	a/symlink' '
	test_check_ignore "a/symlink" 1
'

test_expect_success_multi SYMLINKS 'beyond a symlink' '' '
	test_check_ignore "a/symlink/foo" 128 &&
	test_stderr "fatal: pathspec '\''a/symlink/foo'\'' is beyond a symbolic link"
'

test_expect_success_multi SYMLINKS 'beyond a symlink from subdirectory' '' '
	(
		cd a &&
		test_check_ignore "symlink/foo" 128
	) &&
	test_stderr "fatal: pathspec '\''symlink/foo'\'' is beyond a symbolic link"
'

############################################################################
#
# test handling of submodules

test_expect_success_multi 'submodule' '' '
	test_check_ignore "a/submodule/one" 128 &&
	test_stderr "fatal: Pathspec '\''a/submodule/one'\'' is in submodule '\''a/submodule'\''"
'

test_expect_success_multi 'submodule from subdirectory' '' '
	(
		cd a &&
		test_check_ignore "submodule/one" 128
	) &&
	test_stderr "fatal: Pathspec '\''submodule/one'\'' is in submodule '\''a/submodule'\''"
'

############################################################################
#
# test handling of global ignore files

test_expect_success 'global ignore not yet enabled' '
	expect_from_stdin <<-\EOF &&
		.git/info/exclude:7:per-repo	per-repo
		a/.gitignore:2:*three	a/globalthree
		.git/info/exclude:7:per-repo	a/per-repo
	EOF
	test_check_ignore "-v globalone per-repo a/globalthree a/per-repo not-ignored a/globaltwo"
'

test_expect_success 'global ignore' '
	enable_global_excludes &&
	expect_from_stdin <<-\EOF &&
		globalone
		per-repo
		globalthree
		a/globalthree
		a/per-repo
		globaltwo
	EOF
	test_check_ignore "globalone per-repo globalthree a/globalthree a/per-repo not-ignored globaltwo"
'

test_expect_success 'global ignore with -v' '
	enable_global_excludes &&
	expect_from_stdin <<-EOF &&
		$global_excludes:1:globalone	globalone
		.git/info/exclude:7:per-repo	per-repo
		$global_excludes:3:globalthree	globalthree
		a/.gitignore:2:*three	a/globalthree
		.git/info/exclude:7:per-repo	a/per-repo
		$global_excludes:2:!globaltwo	globaltwo
	EOF
	test_check_ignore "-v globalone per-repo globalthree a/globalthree a/per-repo not-ignored globaltwo"
'

############################################################################
#
# test --stdin

cat <<-\EOF >stdin
	one
	not-ignored
	a/one
	a/not-ignored
	a/b/on
	a/b/one
	a/b/one one
	"a/b/one two"
	"a/b/one\"three"
	a/b/not-ignored
	a/b/two
	a/b/twooo
	globaltwo
	a/globaltwo
	a/b/globaltwo
	b/globaltwo
EOF
cat <<-\EOF >expected-default
	one
	a/one
	a/b/on
	a/b/one
	a/b/one one
	a/b/one two
	"a/b/one\"three"
	a/b/two
	a/b/twooo
	globaltwo
	a/globaltwo
	a/b/globaltwo
	b/globaltwo
EOF
cat <<-EOF >expected-verbose
	.gitignore:1:one	one
	.gitignore:1:one	a/one
	a/b/.gitignore:8:!on*	a/b/on
	a/b/.gitignore:8:!on*	a/b/one
	a/b/.gitignore:8:!on*	a/b/one one
	a/b/.gitignore:8:!on*	a/b/one two
	a/b/.gitignore:8:!on*	"a/b/one\"three"
	a/b/.gitignore:9:!two	a/b/two
	a/.gitignore:1:two*	a/b/twooo
	$global_excludes:2:!globaltwo	globaltwo
	$global_excludes:2:!globaltwo	a/globaltwo
	$global_excludes:2:!globaltwo	a/b/globaltwo
	$global_excludes:2:!globaltwo	b/globaltwo
EOF

sed -e 's/^"//' -e 's/\\//' -e 's/"$//' stdin | \
	tr "\n" "\0" >stdin0
sed -e 's/^"//' -e 's/\\//' -e 's/"$//' expected-default | \
	tr "\n" "\0" >expected-default0
sed -e 's/	"/	/' -e 's/\\//' -e 's/"$//' expected-verbose | \
	tr ":\t\n" "\0" >expected-verbose0

test_expect_success '--stdin' '
	expect_from_stdin <expected-default &&
	test_check_ignore "--stdin" <stdin
'

test_expect_success '--stdin -q' '
	expect "" &&
	test_check_ignore "-q --stdin" <stdin
'

test_expect_success '--stdin -v' '
	expect_from_stdin <expected-verbose &&
	test_check_ignore "-v --stdin" <stdin
'

for opts in '--stdin -z' '-z --stdin'
do
	test_expect_success "$opts" "
		expect_from_stdin <expected-default0 &&
		test_check_ignore '$opts' <stdin0
	"

	test_expect_success "$opts -q" "
		expect "" &&
		test_check_ignore '-q $opts' <stdin0
	"

	test_expect_success "$opts -v" "
		expect_from_stdin <expected-verbose0 &&
		test_check_ignore '-v $opts' <stdin0
	"
done

cat <<-\EOF >stdin
	../one
	../not-ignored
	one
	not-ignored
	b/on
	b/one
	b/one one
	"b/one two"
	"b/one\"three"
	b/two
	b/not-ignored
	b/twooo
	../globaltwo
	globaltwo
	b/globaltwo
	../b/globaltwo
	c/not-ignored
EOF
# N.B. we deliberately end STDIN with a non-matching pattern in order
# to test that the exit code indicates that one or more of the
# provided paths is ignored - in other words, that it represents an
# aggregation of all the results, not just the final result.

cat <<-EOF >expected-all
	.gitignore:1:one	../one
	::	../not-ignored
	.gitignore:1:one	one
	::	not-ignored
	a/b/.gitignore:8:!on*	b/on
	a/b/.gitignore:8:!on*	b/one
	a/b/.gitignore:8:!on*	b/one one
	a/b/.gitignore:8:!on*	b/one two
	a/b/.gitignore:8:!on*	"b/one\"three"
	a/b/.gitignore:9:!two	b/two
	::	b/not-ignored
	a/.gitignore:1:two*	b/twooo
	$global_excludes:2:!globaltwo	../globaltwo
	$global_excludes:2:!globaltwo	globaltwo
	$global_excludes:2:!globaltwo	b/globaltwo
	$global_excludes:2:!globaltwo	../b/globaltwo
	::	c/not-ignored
EOF
grep -v '^::	' expected-all >expected-verbose
sed -e 's/.*	//' expected-verbose >expected-default

sed -e 's/^"//' -e 's/\\//' -e 's/"$//' stdin | \
	tr "\n" "\0" >stdin0
sed -e 's/^"//' -e 's/\\//' -e 's/"$//' expected-default | \
	tr "\n" "\0" >expected-default0
sed -e 's/	"/	/' -e 's/\\//' -e 's/"$//' expected-verbose | \
	tr ":\t\n" "\0" >expected-verbose0

test_expect_success '--stdin from subdirectory' '
	expect_from_stdin <expected-default &&
	(
		cd a &&
		test_check_ignore "--stdin" <../stdin
	)
'

test_expect_success '--stdin from subdirectory with -v' '
	expect_from_stdin <expected-verbose &&
	(
		cd a &&
		test_check_ignore "--stdin -v" <../stdin
	)
'

test_expect_success '--stdin from subdirectory with -v -n' '
	expect_from_stdin <expected-all &&
	(
		cd a &&
		test_check_ignore "--stdin -v -n" <../stdin
	)
'

for opts in '--stdin -z' '-z --stdin'
do
	test_expect_success "$opts from subdirectory" '
		expect_from_stdin <expected-default0 &&
		(
			cd a &&
			test_check_ignore "'"$opts"'" <../stdin0
		)
	'

	test_expect_success "$opts from subdirectory with -v" '
		expect_from_stdin <expected-verbose0 &&
		(
			cd a &&
			test_check_ignore "'"$opts"' -v" <../stdin0
		)
	'
done

test_expect_success PIPE 'streaming support for --stdin' '
	mkfifo in out &&
	(git check-ignore -n -v --stdin <in >out &) &&

	# We cannot just "echo >in" because check-ignore would get EOF
	# after echo exited; instead we open the descriptor in our
	# shell, and then echo to the fd. We make sure to close it at
	# the end, so that the subprocess does get EOF and dies
	# properly.
	#
	# Similarly, we must keep "out" open so that check-ignore does
	# not ever get SIGPIPE trying to write to us. Not only would that
	# produce incorrect results, but then there would be no writer on the
	# other end of the pipe, and we would potentially block forever trying
	# to open it.
	exec 9>in &&
	exec 8<out &&
	test_when_finished "exec 9>&-" &&
	test_when_finished "exec 8<&-" &&
	echo >&9 one &&
	read response <&8 &&
	echo "$response" | grep "^\.gitignore:1:one	one" &&
	echo >&9 two &&
	read response <&8 &&
	echo "$response" | grep "^::	two"
'

test_done
