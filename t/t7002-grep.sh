#!/bin/sh
#
# Copyright (c) 2006 Junio C Hamano
#

test_description='git grep various.
'

. ./test-lib.sh

test_expect_success 'Check for external grep support' '
	case "$(git grep -h 2>&1|grep ext-grep)" in
	*"(default)"*)
		test_set_prereq EXTGREP
		true;;
	*"(ignored by this build)"*)
		true;;
	*)
		false;;
	esac
'

cat >hello.c <<EOF
#include <stdio.h>
int main(int argc, const char **argv)
{
	printf("Hello world.\n");
	return 0;
	/* char ?? */
}
EOF

test_expect_success setup '
	{
		echo foo mmap bar
		echo foo_mmap bar
		echo foo_mmap bar mmap
		echo foo mmap bar_mmap
		echo foo_mmap bar mmap baz
	} >file &&
	echo vvv >v &&
	echo ww w >w &&
	echo x x xx x >x &&
	echo y yy >y &&
	echo zzz > z &&
	mkdir t &&
	echo test >t/t &&
	echo vvv >t/v &&
	mkdir t/a &&
	echo vvv >t/a/v &&
	git add . &&
	test_tick &&
	git commit -m initial
'

test_expect_success 'grep should not segfault with a bad input' '
	test_must_fail git grep "("
'

for H in HEAD ''
do
	case "$H" in
	HEAD)	HC='HEAD:' L='HEAD' ;;
	'')	HC= L='in working tree' ;;
	esac

	test_expect_success "grep -w $L" '
		{
			echo ${HC}file:1:foo mmap bar
			echo ${HC}file:3:foo_mmap bar mmap
			echo ${HC}file:4:foo mmap bar_mmap
			echo ${HC}file:5:foo_mmap bar mmap baz
		} >expected &&
		git grep -n -w -e mmap $H >actual &&
		diff expected actual
	'

	test_expect_success "grep -w $L (w)" '
		: >expected &&
		! git grep -n -w -e "^w" >actual &&
		test_cmp expected actual
	'

	test_expect_success "grep -w $L (x)" '
		{
			echo ${HC}x:1:x x xx x
		} >expected &&
		git grep -n -w -e "x xx* x" $H >actual &&
		diff expected actual
	'

	test_expect_success "grep -w $L (y-1)" '
		{
			echo ${HC}y:1:y yy
		} >expected &&
		git grep -n -w -e "^y" $H >actual &&
		diff expected actual
	'

	test_expect_success "grep -w $L (y-2)" '
		: >expected &&
		if git grep -n -w -e "^y y" $H >actual
		then
			echo should not have matched
			cat actual
			false
		else
			diff expected actual
		fi
	'

	test_expect_success "grep -w $L (z)" '
		: >expected &&
		if git grep -n -w -e "^z" $H >actual
		then
			echo should not have matched
			cat actual
			false
		else
			diff expected actual
		fi
	'

	test_expect_success "grep $L (t-1)" '
		echo "${HC}t/t:1:test" >expected &&
		git grep -n -e test $H >actual &&
		diff expected actual
	'

	test_expect_success "grep $L (t-2)" '
		echo "${HC}t:1:test" >expected &&
		(
			cd t &&
			git grep -n -e test $H
		) >actual &&
		diff expected actual
	'

	test_expect_success "grep $L (t-3)" '
		echo "${HC}t/t:1:test" >expected &&
		(
			cd t &&
			git grep --full-name -n -e test $H
		) >actual &&
		diff expected actual
	'

	test_expect_success "grep -c $L (no /dev/null)" '
		! git grep -c test $H | grep /dev/null
        '

	test_expect_success "grep --max-depth -1 $L" '
		{
			echo ${HC}t/a/v:1:vvv
			echo ${HC}t/v:1:vvv
			echo ${HC}v:1:vvv
		} >expected &&
		git grep --max-depth -1 -n -e vvv $H >actual &&
		test_cmp expected actual
	'

	test_expect_success "grep --max-depth 0 $L" '
		{
			echo ${HC}v:1:vvv
		} >expected &&
		git grep --max-depth 0 -n -e vvv $H >actual &&
		test_cmp expected actual
	'

	test_expect_success "grep --max-depth 0 -- '*' $L" '
		{
			echo ${HC}t/a/v:1:vvv
			echo ${HC}t/v:1:vvv
			echo ${HC}v:1:vvv
		} >expected &&
		git grep --max-depth 0 -n -e vvv $H -- "*" >actual &&
		test_cmp expected actual
	'

	test_expect_success "grep --max-depth 1 $L" '
		{
			echo ${HC}t/v:1:vvv
			echo ${HC}v:1:vvv
		} >expected &&
		git grep --max-depth 1 -n -e vvv $H >actual &&
		test_cmp expected actual
	'

	test_expect_success "grep --max-depth 0 -- t $L" '
		{
			echo ${HC}t/v:1:vvv
		} >expected &&
		git grep --max-depth 0 -n -e vvv $H -- t >actual &&
		test_cmp expected actual
	'

done

cat >expected <<EOF
file:foo mmap bar_mmap
EOF

test_expect_success 'grep -e A --and -e B' '
	git grep -e "foo mmap" --and -e bar_mmap >actual &&
	test_cmp expected actual
'

cat >expected <<EOF
file:foo_mmap bar mmap
file:foo_mmap bar mmap baz
EOF


test_expect_success 'grep ( -e A --or -e B ) --and -e B' '
	git grep \( -e foo_ --or -e baz \) \
		--and -e " mmap" >actual &&
	test_cmp expected actual
'

cat >expected <<EOF
file:foo mmap bar
EOF

test_expect_success 'grep -e A --and --not -e B' '
	git grep -e "foo mmap" --and --not -e bar_mmap >actual &&
	test_cmp expected actual
'

test_expect_success 'grep should ignore GREP_OPTIONS' '
	GREP_OPTIONS=-v git grep " mmap bar\$" >actual &&
	test_cmp expected actual
'

test_expect_success 'grep -f, non-existent file' '
	test_must_fail git grep -f patterns
'

cat >expected <<EOF
file:foo mmap bar
file:foo_mmap bar
file:foo_mmap bar mmap
file:foo mmap bar_mmap
file:foo_mmap bar mmap baz
EOF

cat >pattern <<EOF
mmap
EOF

test_expect_success 'grep -f, one pattern' '
	git grep -f pattern >actual &&
	test_cmp expected actual
'

cat >expected <<EOF
file:foo mmap bar
file:foo_mmap bar
file:foo_mmap bar mmap
file:foo mmap bar_mmap
file:foo_mmap bar mmap baz
t/a/v:vvv
t/v:vvv
v:vvv
EOF

cat >patterns <<EOF
mmap
vvv
EOF

test_expect_success 'grep -f, multiple patterns' '
	git grep -f patterns >actual &&
	test_cmp expected actual
'

cat >expected <<EOF
file:foo mmap bar
file:foo_mmap bar
file:foo_mmap bar mmap
file:foo mmap bar_mmap
file:foo_mmap bar mmap baz
t/a/v:vvv
t/v:vvv
v:vvv
EOF

cat >patterns <<EOF

mmap

vvv

EOF

test_expect_success 'grep -f, ignore empty lines' '
	git grep -f patterns >actual &&
	test_cmp expected actual
'

cat >expected <<EOF
y:y yy
--
z:zzz
EOF

# Create 1024 file names that sort between "y" and "z" to make sure
# the two files are handled by different calls to an external grep.
# This depends on MAXARGS in builtin-grep.c being 1024 or less.
c32="0 1 2 3 4 5 6 7 8 9 a b c d e f g h i j k l m n o p q r s t u v"
test_expect_success 'grep -C1, hunk mark between files' '
	for a in $c32; do for b in $c32; do : >y-$a$b; done; done &&
	git add y-?? &&
	git grep -C1 "^[yz]" >actual &&
	test_cmp expected actual
'

test_expect_success 'grep -C1 --no-ext-grep, hunk mark between files' '
	git grep -C1 --no-ext-grep "^[yz]" >actual &&
	test_cmp expected actual
'

test_expect_success 'log grep setup' '
	echo a >>file &&
	test_tick &&
	GIT_AUTHOR_NAME="With * Asterisk" \
	GIT_AUTHOR_EMAIL="xyzzy@frotz.com" \
	git commit -a -m "second" &&

	echo a >>file &&
	test_tick &&
	git commit -a -m "third"

'

test_expect_success 'log grep (1)' '
	git log --author=author --pretty=tformat:%s >actual &&
	( echo third ; echo initial ) >expect &&
	test_cmp expect actual
'

test_expect_success 'log grep (2)' '
	git log --author=" * " -F --pretty=tformat:%s >actual &&
	( echo second ) >expect &&
	test_cmp expect actual
'

test_expect_success 'log grep (3)' '
	git log --author="^A U" --pretty=tformat:%s >actual &&
	( echo third ; echo initial ) >expect &&
	test_cmp expect actual
'

test_expect_success 'log grep (4)' '
	git log --author="frotz\.com>$" --pretty=tformat:%s >actual &&
	( echo second ) >expect &&
	test_cmp expect actual
'

test_expect_success 'log grep (5)' '
	git log --author=Thor -F --grep=Thu --pretty=tformat:%s >actual &&
	( echo third ; echo initial ) >expect &&
	test_cmp expect actual
'

test_expect_success 'log grep (6)' '
	git log --author=-0700  --pretty=tformat:%s >actual &&
	>expect &&
	test_cmp expect actual
'

test_expect_success 'grep with CE_VALID file' '
	git update-index --assume-unchanged t/t &&
	rm t/t &&
	test "$(git grep --no-ext-grep test)" = "t/t:test" &&
	git update-index --no-assume-unchanged t/t &&
	git checkout t/t
'

cat >expected <<EOF
hello.c=#include <stdio.h>
hello.c:	return 0;
EOF

test_expect_success 'grep -p with userdiff' '
	git config diff.custom.funcname "^#" &&
	echo "hello.c diff=custom" >.gitattributes &&
	git grep -p return >actual &&
	test_cmp expected actual
'

cat >expected <<EOF
hello.c=int main(int argc, const char **argv)
hello.c:	return 0;
EOF

test_expect_success 'grep -p' '
	rm -f .gitattributes &&
	git grep -p return >actual &&
	test_cmp expected actual
'

cat >expected <<EOF
hello.c-#include <stdio.h>
hello.c=int main(int argc, const char **argv)
hello.c-{
hello.c-	printf("Hello world.\n");
hello.c:	return 0;
EOF

test_expect_success 'grep -p -B5' '
	git grep -p -B5 return >actual &&
	test_cmp expected actual
'

test_expect_success 'grep from a subdirectory to search wider area (1)' '
	mkdir -p s &&
	(
		cd s && git grep "x x x" ..
	)
'

test_expect_success 'grep from a subdirectory to search wider area (2)' '
	mkdir -p s &&
	(
		cd s || exit 1
		( git grep xxyyzz .. >out ; echo $? >status )
		! test -s out &&
		test 1 = $(cat status)
	)
'

cat >expected <<EOF
hello.c:int main(int argc, const char **argv)
EOF

test_expect_success 'grep -Fi' '
	git grep -Fi "CHAR *" >actual &&
	test_cmp expected actual
'

test_expect_success EXTGREP 'external grep is called' '
	GIT_TRACE=2 git grep foo >/dev/null 2>actual &&
	grep "trace: grep:.*foo" actual >/dev/null
'

test_expect_success EXTGREP 'no external grep when skip-worktree entries exist' '
	git update-index --skip-worktree file &&
	GIT_TRACE=2 git grep foo >/dev/null 2>actual &&
	! grep "trace: grep:" actual >/dev/null &&
	git update-index --no-skip-worktree file
'

test_done
