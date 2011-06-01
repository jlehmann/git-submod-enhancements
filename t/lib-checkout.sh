#!/bin/sh
#
# Helper functions to check if checkout would succeed/fail as expected with
# and without the dry-run option. They also test that the dry-run does not
# write the index and that together with -u it doesn't touch the work tree.
#
checkout_must_succeed () {
    git ls-files -s >pre-dry-run &&
    git diff-files -p >pre-dry-run-wt &&
    git branch -v >pre-dry-run-br &&
    git checkout -n "$@" &&
    git ls-files -s >post-dry-run &&
    git diff-files -p >post-dry-run-wt &&
    git branch -v >post-dry-run-br &&
    test_cmp pre-dry-run post-dry-run &&
    test_cmp pre-dry-run-wt post-dry-run-wt &&
    test_cmp pre-dry-run-br post-dry-run-br &&
    rm pre-dry-run post-dry-run pre-dry-run-wt post-dry-run-wt
    rm pre-dry-run-br post-dry-run-br
    git checkout "$@"
}

checkout_must_fail () {
    git ls-files -s >pre-dry-run &&
    git diff-files -p >pre-dry-run-wt &&
    git branch -v >pre-dry-run-br &&
    test_must_fail git checkout -n "$@" &&
    git ls-files -s >post-dry-run &&
    git diff-files -p >post-dry-run-wt &&
    git branch -v >post-dry-run-br &&
    test_cmp pre-dry-run post-dry-run &&
    test_cmp pre-dry-run-wt post-dry-run-wt &&
    test_cmp pre-dry-run-br post-dry-run-br &&
    rm pre-dry-run post-dry-run pre-dry-run-wt post-dry-run-wt
    rm pre-dry-run-br post-dry-run-br
    test_must_fail git checkout "$@"
}
