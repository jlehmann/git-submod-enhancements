#!/bin/sh

test_description='git apply handling submodules'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-submodule-update.sh

apply_index () {
	git diff --ignore-submodules=dirty "..$1" | git apply --index -
}

test_submodule_switch "apply_index"

apply_3way () {
	git diff --ignore-submodules=dirty "..$1" | git apply --3way -
}

test_submodule_switch "apply_3way"

apply_index_recursive () {
	git diff --ignore-submodules=dirty "..$1" | git apply --recurse-submodules --index -
}

test_submodule_recursive_switch "apply_index_recursive"

apply_3way_recursive () {
	git diff --ignore-submodules=dirty "..$1" | git apply --recurse-submodules --3way -
}

test_submodule_recursive_switch "apply_3way_recursive"

test_done
