#!/bin/sh
#
# Copyright (c) 2005 Junio C Hamano
#
# Fetch one or more remote refs and merge it/them into the current HEAD.

USAGE='[-n | --no-stat] [--[no-]commit] [--[no-]squash] [--[no-]ff] [-s strategy]... [<fetch-options>] <repo> <head>...'
LONG_USAGE='Fetch one or more remote refs and integrate it/them with the current HEAD.'
SUBDIRECTORY_OK=Yes
OPTIONS_SPEC=
. git-sh-setup
. git-sh-i18n
set_reflog_action "pull${1+ $*}"
require_work_tree_exists
cd_to_toplevel


die_conflict () {
    git diff-index --cached --name-status -r --ignore-submodules HEAD --
    if [ $(git config --bool --get advice.resolveConflict || echo true) = "true" ]; then
	die "$(gettext "Pull is not possible because you have unmerged files.
Please, fix them up in the work tree, and then use 'git add/rm <file>'
as appropriate to mark resolution, or use 'git commit -a'.")"
    else
	die "$(gettext "Pull is not possible because you have unmerged files.")"
    fi
}

die_merge () {
    if [ $(git config --bool --get advice.resolveConflict || echo true) = "true" ]; then
	die "$(gettext "You have not concluded your merge (MERGE_HEAD exists).
Please, commit your changes before you can merge.")"
    else
	die "$(gettext "You have not concluded your merge (MERGE_HEAD exists).")"
    fi
}

test -z "$(git ls-files -u)" || die_conflict
test -f "$GIT_DIR/MERGE_HEAD" && die_merge

strategy_args= diffstat= no_commit= squash= no_ff= ff_only=
log_arg= verbosity= progress= recurse_submodules= verify_signatures=
merge_args= edit=

curr_branch=$(git symbolic-ref -q HEAD)
curr_branch_short="${curr_branch#refs/heads/}"

# See if we are configured to rebase by default.
# The value $rebase is, throughout the main part of the code:
#    (empty) - the user did not have any preference
#    true    - the user told us to integrate by rebasing
#    false   - the user told us to integrate by merging
rebase=$(git config --bool branch.$curr_branch_short.rebase)
if test -z "$rebase"
then
	rebase=$(git config --bool pull.rebase)
fi

dry_run=
while :
do
	case "$1" in
	-q|--quiet)
		verbosity="$verbosity -q" ;;
	-v|--verbose)
		verbosity="$verbosity -v" ;;
	--progress)
		progress=--progress ;;
	--no-progress)
		progress=--no-progress ;;
	-n|--no-stat|--no-summary)
		diffstat=--no-stat ;;
	--stat|--summary)
		diffstat=--stat ;;
	--log|--no-log)
		log_arg=$1 ;;
	--no-c|--no-co|--no-com|--no-comm|--no-commi|--no-commit)
		no_commit=--no-commit ;;
	--c|--co|--com|--comm|--commi|--commit)
		no_commit=--commit ;;
	-e|--edit)
		edit=--edit ;;
	--no-edit)
		edit=--no-edit ;;
	--sq|--squ|--squa|--squas|--squash)
		squash=--squash ;;
	--no-sq|--no-squ|--no-squa|--no-squas|--no-squash)
		squash=--no-squash ;;
	--ff)
		no_ff=--ff ;;
	--no-ff)
		no_ff=--no-ff ;;
	--ff-only)
		ff_only=--ff-only ;;
	-s=*|--s=*|--st=*|--str=*|--stra=*|--strat=*|--strate=*|\
		--strateg=*|--strategy=*|\
	-s|--s|--st|--str|--stra|--strat|--strate|--strateg|--strategy)
		case "$#,$1" in
		*,*=*)
			strategy=`expr "z$1" : 'z-[^=]*=\(.*\)'` ;;
		1,*)
			usage ;;
		*)
			strategy="$2"
			shift ;;
		esac
		strategy_args="${strategy_args}-s $strategy "
		;;
	-X*)
		case "$#,$1" in
		1,-X)
			usage ;;
		*,-X)
			xx="-X $(git rev-parse --sq-quote "$2")"
			shift ;;
		*,*)
			xx=$(git rev-parse --sq-quote "$1") ;;
		esac
		merge_args="$merge_args$xx "
		;;
	-r|--r|--re|--reb|--reba|--rebas|--rebase)
		rebase=true
		;;
	--no-r|--no-re|--no-reb|--no-reba|--no-rebas|--no-rebase|\
	-m|--m|--me|--mer|--merg|--merge)
		rebase=false
		;;
	--recurse-submodules)
		recurse_submodules=--recurse-submodules
		;;
	--recurse-submodules=*)
		recurse_submodules="$1"
		;;
	--no-recurse-submodules)
		recurse_submodules=--no-recurse-submodules
		;;
	--verify-signatures)
		verify_signatures=--verify-signatures
		;;
	--no-verify-signatures)
		verify_signatures=--no-verify-signatures
		;;
	--d|--dr|--dry|--dry-|--dry-r|--dry-ru|--dry-run)
		dry_run=--dry-run
		;;
	-h|--help-all)
		usage
		;;
	*)
		# Pass thru anything that may be meant for fetch.
		break
		;;
	esac
	shift
done

error_on_no_merge_candidates () {
	exec >&2
	for opt
	do
		case "$opt" in
		-t|--t|--ta|--tag|--tags)
			echo "Fetching tags only, you probably meant:"
			echo "  git fetch --tags"
			exit 1
		esac
	done

	if test true = "$rebase"
	then
		op_type=rebase
		op_prep=against
	else
		op_type=merge
		op_prep=with
	fi

	curr_branch=${curr_branch#refs/heads/}
	upstream=$(git config "branch.$curr_branch.merge")
	remote=$(git config "branch.$curr_branch.remote")

	if [ $# -gt 1 ]; then
		if [ "$rebase" = true ]; then
			printf "There is no candidate for rebasing against "
		else
			printf "There are no candidates for merging "
		fi
		echo "among the refs that you just fetched."
		echo "Generally this means that you provided a wildcard refspec which had no"
		echo "matches on the remote end."
	elif [ $# -gt 0 ] && [ "$1" != "$remote" ]; then
		echo "You asked to pull from the remote '$1', but did not specify"
		echo "a branch. Because this is not the default configured remote"
		echo "for your current branch, you must specify a branch on the command line."
	elif [ -z "$curr_branch" -o -z "$upstream" ]; then
		. git-parse-remote
		error_on_missing_default_upstream "pull" $op_type $op_prep \
			"git pull <remote> <branch>"
	else
		echo "Your configuration specifies to $op_type $op_prep the ref '${upstream#refs/heads/}'"
		echo "from the remote, but no such ref was fetched."
	fi
	exit 1
}

test true = "$rebase" && {
	if ! git rev-parse -q --verify HEAD >/dev/null
	then
		# On an unborn branch
		if test -f "$GIT_DIR/index"
		then
			die "$(gettext "updating an unborn branch with changes added to the index")"
		fi
	else
		require_clean_work_tree "pull with rebase" "Please commit or stash them."
	fi
	oldremoteref= &&
	test -n "$curr_branch" &&
	. git-parse-remote &&
	remoteref="$(get_remote_merge_branch "$@" 2>/dev/null)" &&
	oldremoteref="$(git rev-parse -q --verify "$remoteref")" &&
	for reflog in $(git rev-list -g $remoteref 2>/dev/null)
	do
		if test "$reflog" = "$(git merge-base $reflog $curr_branch)"
		then
			oldremoteref="$reflog"
			break
		fi
	done
}

orig_head=$(git rev-parse -q --verify HEAD)
git fetch $verbosity $progress $dry_run $recurse_submodules --update-head-ok "$@" || exit 1
test -z "$dry_run" || exit 0

curr_head=$(git rev-parse -q --verify HEAD)
if test -n "$orig_head" && test "$curr_head" != "$orig_head"
then
	# The fetch involved updating the current branch.

	# The working tree and the index file is still based on the
	# $orig_head commit, but we are merging into $curr_head.
	# First update the working tree to match $curr_head.

	eval_gettextln "Warning: fetch updated the current branch head.
Warning: fast-forwarding your working tree from
Warning: commit \$orig_head." >&2
	git update-index -q --refresh
	git read-tree -u -m "$orig_head" "$curr_head" ||
		die "$(eval_gettext "Cannot fast-forward your working tree.
After making sure that you saved anything precious from
$ git diff \$orig_head
output, run
$ git reset --hard
to recover.")"

fi

merge_head=$(sed -e '/	not-for-merge	/d' \
	-e 's/	.*//' "$GIT_DIR"/FETCH_HEAD | \
	tr '\012' ' ')

case "$merge_head" in
'')
	error_on_no_merge_candidates "$@"
	;;
?*' '?*)
	if test -z "$orig_head"
	then
		die "$(gettext "Cannot merge multiple branches into empty head")"
	fi
	if test true = "$rebase"
	then
		die "$(gettext "Cannot rebase onto multiple branches")"
	fi
	;;
*)
	# integrating with a single other history; be careful not to
	# trigger this check when we will say "fast-forward" or "already
	# up-to-date".
	merge_head=${merge_head% }
	if test -z "$rebase$no_ff$ff_only${squash#--no-squash}" &&
		test -n "$orig_head" &&
		test $# = 0 &&
		! git merge-base --is-ancestor "$orig_head" "$merge_head" &&
		! git merge-base --is-ancestor "$merge_head" "$orig_head"
	then
echo >&2 "orig-head was $orig_head"
echo >&2 "merge-head is $merge_head"
git show >&2 --oneline -s "$orig_head" "$merge_head"

		die "The pull does not fast-forward; please specify
if you want to merge or rebase.

Use either

    git pull --rebase
    git pull --merge

You can also use 'git config pull.rebase true' (if you want --rebase) or
'git config pull.rebase false' (if you want --merge) to set this once for
this project and forget about it."
	fi
	;;
esac

# Pulling into unborn branch: a shorthand for branching off
# FETCH_HEAD, for lazy typers.
if test -z "$orig_head"
then
	# Two-way merge: we claim the index is based on an empty tree,
	# and try to fast-forward to HEAD.  This ensures we will not
	# lose index/worktree changes that the user already made on
	# the unborn branch.
	empty_tree=4b825dc642cb6eb9a060e54bf8d69288fbee4904
	git read-tree -m -u $empty_tree $merge_head &&
	git update-ref -m "initial pull" HEAD $merge_head "$curr_head"
	exit
fi

if test true = "$rebase"
then
	o=$(git show-branch --merge-base $curr_branch $merge_head $oldremoteref)
	if test "$oldremoteref" = "$o"
	then
		unset oldremoteref
	fi
fi

merge_name=$(git fmt-merge-msg $log_arg <"$GIT_DIR/FETCH_HEAD") || exit
case "$rebase" in
true)
	eval="git-rebase $diffstat $strategy_args $merge_args $verbosity"
	eval="$eval --onto $merge_head ${oldremoteref:-$merge_head}"
	;;
*)
	eval="git-merge $diffstat $no_commit $verify_signatures $edit $squash $no_ff $ff_only"
	eval="$eval  $log_arg $strategy_args $merge_args $verbosity $progress"
	eval="$eval \"\$merge_name\" HEAD $merge_head"
	;;
esac
eval "exec $eval"
