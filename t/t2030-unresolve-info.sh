#!/bin/sh

test_description='undoing resolution'

. ./test-lib.sh

check_resolve_undo () {
	msg=$1
	shift
	while case $# in
	0)	break ;;
	1|2|3)	die "Bug in check-resolve-undo test" ;;
	esac
	do
		path=$1
		shift
		for stage in 1 2 3
		do
			sha1=$1
			shift
			case "$sha1" in
			'') continue ;;
			esac
			sha1=$(git rev-parse --verify "$sha1")
			printf "100644 %s %s\t%s\n" $sha1 $stage $path
		done
	done >"$msg.expect" &&
	git ls-files --resolve-undo >"$msg.actual" &&
	test_cmp "$msg.expect" "$msg.actual"
}

prime_resolve_undo () {
	git reset --hard &&
	git checkout second^0 &&
	test_tick &&
	test_must_fail git merge third^0 &&
	echo merge does not leave anything &&
	check_resolve_undo empty &&
	echo different >file &&
	git add file &&
	echo resolving records &&
	check_resolve_undo recorded file initial:file second:file third:file
}

test_expect_success setup '
	test_commit initial file first &&
	git branch side &&
	git branch another &&
	test_commit second file second &&
	git checkout side &&
	test_commit third file third &&
	git checkout another &&
	test_commit fourth file fourth &&
	git checkout master
'

test_expect_success 'add records switch clears' '
	prime_resolve_undo &&
	test_tick &&
	git commit -m merged &&
	echo committing keeps &&
	check_resolve_undo kept file initial:file second:file third:file &&
	git checkout second^0 &&
	echo switching clears &&
	check_resolve_undo cleared
'

test_expect_success 'rm records reset clears' '
	prime_resolve_undo &&
	test_tick &&
	git commit -m merged &&
	echo committing keeps &&
	check_resolve_undo kept file initial:file second:file third:file &&

	echo merge clears upfront &&
	test_must_fail git merge fourth^0 &&
	check_resolve_undo nuked &&

	git rm -f file &&
	echo resolving records &&
	check_resolve_undo recorded file initial:file HEAD:file fourth:file &&

	git reset --hard &&
	echo resetting discards &&
	check_resolve_undo discarded
'

test_expect_success 'plumbing clears' '
	prime_resolve_undo &&
	test_tick &&
	git commit -m merged &&
	echo committing keeps &&
	check_resolve_undo kept file initial:file second:file third:file &&

	echo plumbing clear &&
	git update-index --clear-resolve-undo &&
	check_resolve_undo cleared
'

test_expect_success 'add records checkout -m undoes' '
	prime_resolve_undo &&
	git diff HEAD &&
	git checkout --conflict=merge file &&
	echo checkout used the record and removed it &&
	check_resolve_undo removed &&
	echo the index and the work tree is unmerged again &&
	git diff >actual &&
	grep "^++<<<<<<<" actual
'

test_expect_success 'unmerge with plumbing' '
	prime_resolve_undo &&
	git update-index --unresolve file &&
	git ls-files -u >actual &&
	test $(wc -l <actual) = 3
'

test_expect_success 'rerere and rerere --forget' '
	mkdir .git/rr-cache &&
	prime_resolve_undo &&
	echo record the resolution &&
	git rerere &&
	rerere_id=$(cd .git/rr-cache && echo */postimage) &&
	rerere_id=${rerere_id%/postimage} &&
	test -f .git/rr-cache/$rerere_id/postimage &&
	git checkout -m file &&
	echo resurrect the conflict &&
	grep "^=======" file &&
	echo reresolve the conflict &&
	git rerere &&
	test "z$(cat file)" = zdifferent &&
	echo register the resolution again &&
	git add file &&
	check_resolve_undo kept file initial:file second:file third:file &&
	test -z "$(git ls-files -u)" &&
	git rerere forget file &&
	! test -f .git/rr-cache/$rerere_id/postimage &&
	tr "\0" "\n" <.git/MERGE_RR >actual &&
	echo "$rerere_id	file" >expect &&
	test_cmp expect actual
'

test_done
