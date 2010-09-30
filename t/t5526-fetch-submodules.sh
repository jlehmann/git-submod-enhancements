#!/bin/sh
# Copyright (c) 2010, Jens Lehmann

test_description='Recursive "git fetch" for submodules'

. ./test-lib.sh

pwd=$(pwd)

add_upstream_commit() {
	(
		cd submodule &&
		head1=$(git rev-parse --short HEAD) &&
		echo new >> subfile &&
		test_tick &&
		git add subfile &&
		git commit -m new subfile &&
		head2=$(git rev-parse --short HEAD) &&
		echo "From $pwd/submodule" > ../expect_1st.err &&
		echo "   $head1..$head2  master     -> origin/master" >> ../expect_1st.err
	)
	(
		cd deepsubmodule &&
		head1=$(git rev-parse --short HEAD) &&
		echo new >> deepsubfile &&
		test_tick &&
		git add deepsubfile &&
		git commit -m new deepsubfile &&
		head2=$(git rev-parse --short HEAD) &&
		echo "From $pwd/deepsubmodule" > ../expect_2nd.err &&
		echo "   $head1..$head2  master     -> origin/master" >> ../expect_2nd.err
	) &&
	cat expect_1st.err expect_2nd.err > expect.err
}

test_expect_success setup '
	mkdir deepsubmodule &&
	(
		cd deepsubmodule &&
		git init &&
		echo deepsubcontent > deepsubfile &&
		git add deepsubfile &&
		git commit -m new deepsubfile
	) &&
	mkdir submodule &&
	(
		cd submodule &&
		git init &&
		echo subcontent > subfile &&
		git add subfile &&
		git submodule add "$pwd/deepsubmodule" deepsubmodule &&
		git commit -a -m new
	) &&
	git submodule add "$pwd/submodule" submodule &&
	git commit -am initial &&
	git clone . downstream &&
	(
		cd downstream &&
		git submodule update --init --recursive
	) &&
	echo "Fetching submodule submodule" > expect.out &&
	cp expect.out expect_1st.out &&
	echo "Fetching submodule submodule/deepsubmodule" >> expect.out
'

test_expect_success "fetch recurses into submodules" '
	add_upstream_commit &&
	(
		cd downstream &&
		git fetch >../actual.out 2>../actual.err
	) &&
	test_cmp expect.out actual.out &&
	test_cmp expect.err actual.err
'

test_expect_success "fetch --no-recursive only fetches superproject" '
	(
		cd downstream &&
		git fetch --no-recursive >../actual.out 2>../actual.err
	) &&
	! test -s actual.out &&
	! test -s actual.err
'

test_expect_success "using fetch=false in .gitmodules only fetches superproject" '
	(
		cd downstream &&
		git config -f .gitmodules submodule.submodule.fetch false &&
		git fetch >../actual.out 2>../actual.err
	) &&
	! test -s actual.out &&
	! test -s actual.err
'

test_expect_success "--recursive overrides .gitmodules config" '
	add_upstream_commit &&
	(
		cd downstream &&
		git fetch --recursive >../actual.out 2>../actual.err
	) &&
	test_cmp expect.out actual.out &&
	test_cmp expect.err actual.err
'

test_expect_success "using fetch=true in .git/config overrides setting in .gitmodules" '
	add_upstream_commit &&
	(
		cd downstream &&
		git config submodule.submodule.fetch true &&
		git fetch >../actual.out 2>../actual.err
	) &&
	test_cmp expect.out actual.out &&
	test_cmp expect.err actual.err
'

test_expect_success "--no-recursive overrides fetch setting from .git/config" '
	add_upstream_commit &&
	(
		cd downstream &&
		git fetch --no-recursive >../actual.out 2>../actual.err
	) &&
	! test -s actual.out &&
	! test -s actual.err
'

test_expect_success "--quiet propagates to submodules" '
	(
		cd downstream &&
		git fetch --quiet >../actual.out 2>../actual.err
	) &&
	! test -s actual.out &&
	! test -s actual.err
'

test_expect_success "--dry-run propagates to submodules" '
	add_upstream_commit &&
	(
		cd downstream &&
		git fetch --dry-run >../actual.out 2>../actual.err
	) &&
	test_cmp expect.out actual.out &&
	test_cmp expect.err actual.err &&
	(
		cd downstream &&
		git fetch >../actual.out 2>../actual.err
	) &&
	test_cmp expect.out actual.out &&
	test_cmp expect.err actual.err
'

test_expect_success "--recursive propagates to submodules" '
	add_upstream_commit &&
	(
		cd downstream &&
		(
			cd submodule &&
			git config -f .gitmodules submodule.deepsubmodule.fetch false
		) &&
		git fetch --recursive >../actual.out 2>../actual.err
	) &&
	test_cmp expect.out actual.out &&
	test_cmp expect.err actual.err
'

test_expect_success "fetch.recursive sets default and --recursive overrides it" '
	add_upstream_commit &&
	(
		cd downstream &&
		(
			cd submodule &&
			git config -f .gitmodules --unset submodule.deepsubmodule.fetch &&
			git config fetch.recursive false
		) &&
		git fetch >../actual.out 2>../actual.err
	) &&
	test_cmp expect_1st.out actual.out &&
	test_cmp expect_1st.err actual.err &&
	(
		cd downstream &&
		git fetch --recursive >../actual.out 2>../actual.err
	) &&
	test_cmp expect.out actual.out &&
	test_cmp expect_2nd.err actual.err
'

test_expect_success "fetch setting from .git/config overrides fetch.recursive config setting" '
	add_upstream_commit &&
	(
		cd downstream &&
		git config submodule.submodule.fetch true &&
		git fetch >../actual.out 2>../actual.err
	) &&
	test_cmp expect_1st.out actual.out &&
	test_cmp expect_1st.err actual.err &&
	(
		cd downstream &&
		(
			cd submodule &&
			git config --unset fetch.recursive
		) &&
		git config fetch.recursive false &&
		git config submodule.submodule.fetch true &&
		git fetch >../actual.out 2>../actual.err
	) &&
	test_cmp expect.out actual.out &&
	test_cmp expect_2nd.err actual.err
'

test_done
