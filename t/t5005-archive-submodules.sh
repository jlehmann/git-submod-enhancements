#!/bin/sh

test_description='git archive can include submodule content'

. ./test-lib.sh

add_file()
{
	git add $1 &&
	git commit -m "added $1"
}

add_submodule()
{
	mkdir $1 && (
		cd $1 &&
		git init &&
		echo "File $2" >$2 &&
		add_file $2
	) &&
	add_file $1
}

test_expect_success 'by default, submodules are not included' '
	echo "File 1" >1 &&
	add_file 1 &&
	add_submodule 2 3 &&
	add_submodule 4 5 &&
	cat <<EOF >expected &&
1
2/
4/
EOF
	git archive HEAD >normal.tar &&
	tar -tf normal.tar >actual &&
	test_cmp expected actual
'

test_expect_success 'with --recurse-submodules, checked out submodules are  included' '
	cat <<EOF >expected &&
1
2/
2/3
4/
4/5
EOF
	git archive --recurse-submodules HEAD >full.tar &&
	tar -tf full.tar >actual &&
	test_cmp expected actual
'

test_expect_success 'with --recurse-submodules=all, all submodules are included' '
	git archive --recurse-submodules=all HEAD >all.tar &&
	tar -tf all.tar >actual &&
	test_cmp expected actual
'

test_expect_success 'submodules in submodules are supported' '
	(cd 4 && add_submodule 6 7) &&
	add_file 4 &&
	cat <<EOF >expected &&
1
2/
2/3
4/
4/5
4/6/
4/6/7
EOF
	git archive --recurse-submodules HEAD >recursive.tar &&
	tar -tf recursive.tar >actual &&
	test_cmp expected actual
'

test_expect_success 'packed submodules are supported' '
	msg=$(cd 2 && git repack -ad && git count-objects) &&
	test "$msg" = "0 objects, 0 kilobytes" &&
	git archive --recurse-submodules HEAD >packed.tar &&
	tar -tf packed.tar >actual &&
	test_cmp expected actual
'

test_expect_success 'missing submodule packs triggers an error' '
	mv 2/.git/objects/pack .git/packdir2 &&
	test_must_fail git archive --recurse-submodules HEAD
'

test_expect_success '--recurse-submodules skips non-checked out submodules' '
	cat <<EOF >expected &&
1
2/
4/
4/5
4/6/
4/6/7
EOF
	rm -rf 2/.git &&
	git archive --recurse-submodules HEAD >partial.tar &&
	tar -tf partial.tar >actual &&
	test_cmp expected actual
'

test_expect_success '--recurse-submodules=all fails if gitlinked objects are missing' '
	test_must_fail git archive --recurse-submodules=all HEAD
'

test_expect_success \
	'--recurse-submodules=all does not require submodules to be checked out' '
	cat <<EOF >expected &&
1
2/
2/3
4/
4/5
4/6/
4/6/7
EOF
	mv .git/packdir2/* .git/objects/pack/ &&
	git archive --recurse-submodules=all HEAD >all2.tar &&
	tar -tf all2.tar >actual &&
	test_cmp expected actual
'

test_expect_success 'missing objects in a submodule triggers an error' '
	find 4/.git/objects -type f | xargs rm &&
	test_must_fail git archive --recurse-submodules HEAD
'

test_done
