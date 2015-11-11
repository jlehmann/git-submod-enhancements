#!/bin/sh

test_description='reset can handle submodules'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-submodule-update.sh

test_submodule_switch "git reset --keep"

test_submodule_switch "git reset --merge"

test_submodule_forced_switch "git reset --hard"

test_submodule_recursive_switch "git reset --recurse-submodules --keep"

test_submodule_recursive_switch "git reset --recurse-submodules --merge"

test_submodule_forced_recursive_switch "git reset --recurse-submodules --hard"

test_done
