#!/bin/sh
#
# This is included in commands that either have to be run from the toplevel
# of the repository, or with GIT_DIR environment variable properly.
# If the GIT_DIR does not look like the right correct git-repository,
# it dies.

# Having this variable in your environment would break scripts because
# you would cause "cd" to be taken to unexpected places.  If you
# like CDPATH, define it for your interactive shell sessions without
# exporting it.
unset CDPATH

git_broken_path_fix () {
	case ":$PATH:" in
	*:$1:*) : ok ;;
	*)
		PATH=$(
			SANE_TOOL_PATH="$1"
			IFS=: path= sep=
			set x $PATH
			shift
			for elem
			do
				case "$SANE_TOOL_PATH:$elem" in
				(?*:/bin | ?*:/usr/bin)
					path="$path$sep$SANE_TOOL_PATH"
					sep=:
					SANE_TOOL_PATH=
				esac
				path="$path$sep$elem"
				sep=:
			done
			echo "$path"
		)
		;;
	esac
}

# @@BROKEN_PATH_FIX@@

die() {
	echo >&2 "$@"
	exit 1
}

GIT_QUIET=

say () {
	if test -z "$GIT_QUIET"
	then
		printf '%s\n' "$*"
	fi
}

if test -n "$OPTIONS_SPEC"; then
	usage() {
		"$0" -h
		exit 1
	}

	parseopt_extra=
	[ -n "$OPTIONS_KEEPDASHDASH" ] &&
		parseopt_extra="--keep-dashdash"

	eval "$(
		echo "$OPTIONS_SPEC" |
			git rev-parse --parseopt $parseopt_extra -- "$@" ||
		echo exit $?
	)"
else
	dashless=$(basename "$0" | sed -e 's/-/ /')
	usage() {
		die "Usage: $dashless $USAGE"
	}

	if [ -z "$LONG_USAGE" ]
	then
		LONG_USAGE="Usage: $dashless $USAGE"
	else
		LONG_USAGE="Usage: $dashless $USAGE

$LONG_USAGE"
	fi

	case "$1" in
		-h|--h|--he|--hel|--help)
		echo "$LONG_USAGE"
		exit
	esac
fi

set_reflog_action() {
	if [ -z "${GIT_REFLOG_ACTION:+set}" ]
	then
		GIT_REFLOG_ACTION="$*"
		export GIT_REFLOG_ACTION
	fi
}

git_editor() {
	if test -z "${GIT_EDITOR:+set}"
	then
		GIT_EDITOR="$(git var GIT_EDITOR)" || return $?
	fi

	eval "$GIT_EDITOR" '"$@"'
}

sane_grep () {
	GREP_OPTIONS= LC_ALL=C grep "$@"
}

sane_egrep () {
	GREP_OPTIONS= LC_ALL=C egrep "$@"
}

is_bare_repository () {
	git rev-parse --is-bare-repository
}

cd_to_toplevel () {
	cdup=$(git rev-parse --show-toplevel) &&
	cd "$cdup" || {
		echo >&2 "Cannot chdir to $cdup, the toplevel of the working tree"
		exit 1
	}
}

require_work_tree () {
	test $(git rev-parse --is-inside-work-tree) = true ||
	die "fatal: $0 cannot be used without a working tree."
}

get_author_ident_from_commit () {
	pick_author_script='
	/^author /{
		s/'\''/'\''\\'\'\''/g
		h
		s/^author \([^<]*\) <[^>]*> .*$/\1/
		s/'\''/'\''\'\'\''/g
		s/.*/GIT_AUTHOR_NAME='\''&'\''/p

		g
		s/^author [^<]* <\([^>]*\)> .*$/\1/
		s/'\''/'\''\'\'\''/g
		s/.*/GIT_AUTHOR_EMAIL='\''&'\''/p

		g
		s/^author [^<]* <[^>]*> \(.*\)$/\1/
		s/'\''/'\''\'\'\''/g
		s/.*/GIT_AUTHOR_DATE='\''&'\''/p

		q
	}
	'
	encoding=$(git config i18n.commitencoding || echo UTF-8)
	git show -s --pretty=raw --encoding="$encoding" "$1" -- |
	LANG=C LC_ALL=C sed -ne "$pick_author_script"
}

# Make sure we are in a valid repository of a vintage we understand,
# if we require to be in a git repository.
if test -z "$NONGIT_OK"
then
	GIT_DIR=$(git rev-parse --git-dir) || exit
	if [ -z "$SUBDIRECTORY_OK" ]
	then
		test -z "$(git rev-parse --show-cdup)" || {
			exit=$?
			echo >&2 "You need to run this command from the toplevel of the working tree."
			exit $exit
		}
	fi
	test -n "$GIT_DIR" && GIT_DIR=$(cd "$GIT_DIR" && pwd) || {
		echo >&2 "Unable to determine absolute path of git directory"
		exit 1
	}
	: ${GIT_OBJECT_DIRECTORY="$GIT_DIR/objects"}
fi

# Fix some commands on Windows
case $(uname -s) in
*MINGW*)
	# Windows has its own (incompatible) sort and find
	sort () {
		/usr/bin/sort "$@"
	}
	find () {
		/usr/bin/find "$@"
	}
	;;
esac
