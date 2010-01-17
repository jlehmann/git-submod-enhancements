#include "cache.h"
#include "commit.h"
#include "refs.h"
#include "pkt-line.h"
#include "sideband.h"
#include "run-command.h"
#include "remote.h"
#include "send-pack.h"
#include "quote.h"

static const char send_pack_usage[] =
"git send-pack [--all | --mirror] [--dry-run] [--force] [--receive-pack=<git-receive-pack>] [--verbose] [--thin] [<host>:]<directory> [<ref>...]\n"
"  --all and explicit <ref> specification are mutually exclusive.";

static struct send_pack_args args;

static int feed_object(const unsigned char *sha1, int fd, int negative)
{
	char buf[42];

	if (negative && !has_sha1_file(sha1))
		return 1;

	memcpy(buf + negative, sha1_to_hex(sha1), 40);
	if (negative)
		buf[0] = '^';
	buf[40 + negative] = '\n';
	return write_or_whine(fd, buf, 41 + negative, "send-pack: send refs");
}

/*
 * Make a pack stream and spit it out into file descriptor fd
 */
static int pack_objects(int fd, struct ref *refs, struct extra_have_objects *extra, struct send_pack_args *args)
{
	/*
	 * The child becomes pack-objects --revs; we feed
	 * the revision parameters to it via its stdin and
	 * let its stdout go back to the other end.
	 */
	const char *argv[] = {
		"pack-objects",
		"--all-progress-implied",
		"--revs",
		"--stdout",
		NULL,
		NULL,
		NULL,
		NULL,
	};
	struct child_process po;
	int i;

	i = 4;
	if (args->use_thin_pack)
		argv[i++] = "--thin";
	if (args->use_ofs_delta)
		argv[i++] = "--delta-base-offset";
	if (args->quiet)
		argv[i++] = "-q";
	memset(&po, 0, sizeof(po));
	po.argv = argv;
	po.in = -1;
	po.out = args->stateless_rpc ? -1 : fd;
	po.git_cmd = 1;
	if (start_command(&po))
		die_errno("git pack-objects failed");

	/*
	 * We feed the pack-objects we just spawned with revision
	 * parameters by writing to the pipe.
	 */
	for (i = 0; i < extra->nr; i++)
		if (!feed_object(extra->array[i], po.in, 1))
			break;

	while (refs) {
		if (!is_null_sha1(refs->old_sha1) &&
		    !feed_object(refs->old_sha1, po.in, 1))
			break;
		if (!is_null_sha1(refs->new_sha1) &&
		    !feed_object(refs->new_sha1, po.in, 0))
			break;
		refs = refs->next;
	}

	close(po.in);

	if (args->stateless_rpc) {
		char *buf = xmalloc(LARGE_PACKET_MAX);
		while (1) {
			ssize_t n = xread(po.out, buf, LARGE_PACKET_MAX);
			if (n <= 0)
				break;
			send_sideband(fd, -1, buf, n, LARGE_PACKET_MAX);
		}
		free(buf);
		close(po.out);
		po.out = -1;
	}

	if (finish_command(&po))
		return error("pack-objects died with strange error");
	return 0;
}

static int receive_status(int in, struct ref *refs)
{
	struct ref *hint;
	char line[1000];
	int ret = 0;
	int len = packet_read_line(in, line, sizeof(line));
	if (len < 10 || memcmp(line, "unpack ", 7))
		return error("did not receive remote status");
	if (memcmp(line, "unpack ok\n", 10)) {
		char *p = line + strlen(line) - 1;
		if (*p == '\n')
			*p = '\0';
		error("unpack failed: %s", line + 7);
		ret = -1;
	}
	hint = NULL;
	while (1) {
		char *refname;
		char *msg;
		len = packet_read_line(in, line, sizeof(line));
		if (!len)
			break;
		if (len < 3 ||
		    (memcmp(line, "ok ", 3) && memcmp(line, "ng ", 3))) {
			fprintf(stderr, "protocol error: %s\n", line);
			ret = -1;
			break;
		}

		line[strlen(line)-1] = '\0';
		refname = line + 3;
		msg = strchr(refname, ' ');
		if (msg)
			*msg++ = '\0';

		/* first try searching at our hint, falling back to all refs */
		if (hint)
			hint = find_ref_by_name(hint, refname);
		if (!hint)
			hint = find_ref_by_name(refs, refname);
		if (!hint) {
			warning("remote reported status on unknown ref: %s",
					refname);
			continue;
		}
		if (hint->status != REF_STATUS_EXPECTING_REPORT) {
			warning("remote reported status on unexpected ref: %s",
					refname);
			continue;
		}

		if (line[0] == 'o' && line[1] == 'k')
			hint->status = REF_STATUS_OK;
		else {
			hint->status = REF_STATUS_REMOTE_REJECT;
			ret = -1;
		}
		if (msg)
			hint->remote_status = xstrdup(msg);
		/* start our next search from the next ref */
		hint = hint->next;
	}
	return ret;
}

static void update_tracking_ref(struct remote *remote, struct ref *ref)
{
	struct refspec rs;

	if (ref->status != REF_STATUS_OK && ref->status != REF_STATUS_UPTODATE)
		return;

	rs.src = ref->name;
	rs.dst = NULL;

	if (!remote_find_tracking(remote, &rs)) {
		if (args.verbose)
			fprintf(stderr, "updating local tracking ref '%s'\n", rs.dst);
		if (ref->deletion) {
			delete_ref(rs.dst, NULL, 0);
		} else
			update_ref("update by push", rs.dst,
					ref->new_sha1, NULL, 0, 0);
		free(rs.dst);
	}
}

#define SUMMARY_WIDTH (2 * DEFAULT_ABBREV + 3)

static void print_ref_status(char flag, const char *summary, struct ref *to, struct ref *from, const char *msg)
{
	fprintf(stderr, " %c %-*s ", flag, SUMMARY_WIDTH, summary);
	if (from)
		fprintf(stderr, "%s -> %s", prettify_refname(from->name), prettify_refname(to->name));
	else
		fputs(prettify_refname(to->name), stderr);
	if (msg) {
		fputs(" (", stderr);
		fputs(msg, stderr);
		fputc(')', stderr);
	}
	fputc('\n', stderr);
}

static const char *status_abbrev(unsigned char sha1[20])
{
	return find_unique_abbrev(sha1, DEFAULT_ABBREV);
}

static void print_ok_ref_status(struct ref *ref)
{
	if (ref->deletion)
		print_ref_status('-', "[deleted]", ref, NULL, NULL);
	else if (is_null_sha1(ref->old_sha1))
		print_ref_status('*',
			(!prefixcmp(ref->name, "refs/tags/") ? "[new tag]" :
			  "[new branch]"),
			ref, ref->peer_ref, NULL);
	else {
		char quickref[84];
		char type;
		const char *msg;

		strcpy(quickref, status_abbrev(ref->old_sha1));
		if (ref->nonfastforward) {
			strcat(quickref, "...");
			type = '+';
			msg = "forced update";
		} else {
			strcat(quickref, "..");
			type = ' ';
			msg = NULL;
		}
		strcat(quickref, status_abbrev(ref->new_sha1));

		print_ref_status(type, quickref, ref, ref->peer_ref, msg);
	}
}

static int print_one_push_status(struct ref *ref, const char *dest, int count)
{
	if (!count)
		fprintf(stderr, "To %s\n", dest);

	switch(ref->status) {
	case REF_STATUS_NONE:
		print_ref_status('X', "[no match]", ref, NULL, NULL);
		break;
	case REF_STATUS_REJECT_NODELETE:
		print_ref_status('!', "[rejected]", ref, NULL,
				"remote does not support deleting refs");
		break;
	case REF_STATUS_UPTODATE:
		print_ref_status('=', "[up to date]", ref,
				ref->peer_ref, NULL);
		break;
	case REF_STATUS_REJECT_NONFASTFORWARD:
		print_ref_status('!', "[rejected]", ref, ref->peer_ref,
				"non-fast-forward");
		break;
	case REF_STATUS_REMOTE_REJECT:
		print_ref_status('!', "[remote rejected]", ref,
				ref->deletion ? NULL : ref->peer_ref,
				ref->remote_status);
		break;
	case REF_STATUS_EXPECTING_REPORT:
		print_ref_status('!', "[remote failure]", ref,
				ref->deletion ? NULL : ref->peer_ref,
				"remote failed to report status");
		break;
	case REF_STATUS_OK:
		print_ok_ref_status(ref);
		break;
	}

	return 1;
}

static void print_push_status(const char *dest, struct ref *refs)
{
	struct ref *ref;
	int n = 0;

	if (args.verbose) {
		for (ref = refs; ref; ref = ref->next)
			if (ref->status == REF_STATUS_UPTODATE)
				n += print_one_push_status(ref, dest, n);
	}

	for (ref = refs; ref; ref = ref->next)
		if (ref->status == REF_STATUS_OK)
			n += print_one_push_status(ref, dest, n);

	for (ref = refs; ref; ref = ref->next) {
		if (ref->status != REF_STATUS_NONE &&
		    ref->status != REF_STATUS_UPTODATE &&
		    ref->status != REF_STATUS_OK)
			n += print_one_push_status(ref, dest, n);
	}
}

static int refs_pushed(struct ref *ref)
{
	for (; ref; ref = ref->next) {
		switch(ref->status) {
		case REF_STATUS_NONE:
		case REF_STATUS_UPTODATE:
			break;
		default:
			return 1;
		}
	}
	return 0;
}

static void print_helper_status(struct ref *ref)
{
	struct strbuf buf = STRBUF_INIT;

	for (; ref; ref = ref->next) {
		const char *msg = NULL;
		const char *res;

		switch(ref->status) {
		case REF_STATUS_NONE:
			res = "error";
			msg = "no match";
			break;

		case REF_STATUS_OK:
			res = "ok";
			break;

		case REF_STATUS_UPTODATE:
			res = "ok";
			msg = "up to date";
			break;

		case REF_STATUS_REJECT_NONFASTFORWARD:
			res = "error";
			msg = "non-fast forward";
			break;

		case REF_STATUS_REJECT_NODELETE:
		case REF_STATUS_REMOTE_REJECT:
			res = "error";
			break;

		case REF_STATUS_EXPECTING_REPORT:
		default:
			continue;
		}

		strbuf_reset(&buf);
		strbuf_addf(&buf, "%s %s", res, ref->name);
		if (ref->remote_status)
			msg = ref->remote_status;
		if (msg) {
			strbuf_addch(&buf, ' ');
			quote_two_c_style(&buf, "", msg, 0);
		}
		strbuf_addch(&buf, '\n');

		safe_write(1, buf.buf, buf.len);
	}
	strbuf_release(&buf);
}

int send_pack(struct send_pack_args *args,
	      int fd[], struct child_process *conn,
	      struct ref *remote_refs,
	      struct extra_have_objects *extra_have)
{
	int in = fd[0];
	int out = fd[1];
	struct strbuf req_buf = STRBUF_INIT;
	struct ref *ref;
	int new_refs;
	int ask_for_status_report = 0;
	int allow_deleting_refs = 0;
	int expect_status_report = 0;
	int ret;

	/* Does the other end support the reporting? */
	if (server_supports("report-status"))
		ask_for_status_report = 1;
	if (server_supports("delete-refs"))
		allow_deleting_refs = 1;
	if (server_supports("ofs-delta"))
		args->use_ofs_delta = 1;

	if (!remote_refs) {
		fprintf(stderr, "No refs in common and none specified; doing nothing.\n"
			"Perhaps you should specify a branch such as 'master'.\n");
		return 0;
	}

	/*
	 * Finally, tell the other end!
	 */
	new_refs = 0;
	for (ref = remote_refs; ref; ref = ref->next) {
		if (!ref->peer_ref && !args->send_mirror)
			continue;

		/* Check for statuses set by set_ref_status_for_push() */
		switch (ref->status) {
		case REF_STATUS_REJECT_NONFASTFORWARD:
		case REF_STATUS_UPTODATE:
			continue;
		default:
			; /* do nothing */
		}

		if (ref->deletion && !allow_deleting_refs) {
			ref->status = REF_STATUS_REJECT_NODELETE;
			continue;
		}

		if (!ref->deletion)
			new_refs++;

		if (!args->dry_run) {
			char *old_hex = sha1_to_hex(ref->old_sha1);
			char *new_hex = sha1_to_hex(ref->new_sha1);

			if (ask_for_status_report) {
				packet_buf_write(&req_buf, "%s %s %s%c%s",
					old_hex, new_hex, ref->name, 0,
					"report-status");
				ask_for_status_report = 0;
				expect_status_report = 1;
			}
			else
				packet_buf_write(&req_buf, "%s %s %s",
					old_hex, new_hex, ref->name);
		}
		ref->status = expect_status_report ?
			REF_STATUS_EXPECTING_REPORT :
			REF_STATUS_OK;
	}

	if (args->stateless_rpc) {
		if (!args->dry_run) {
			packet_buf_flush(&req_buf);
			send_sideband(out, -1, req_buf.buf, req_buf.len, LARGE_PACKET_MAX);
		}
	} else {
		safe_write(out, req_buf.buf, req_buf.len);
		packet_flush(out);
	}
	strbuf_release(&req_buf);

	if (new_refs && !args->dry_run) {
		if (pack_objects(out, remote_refs, extra_have, args) < 0) {
			for (ref = remote_refs; ref; ref = ref->next)
				ref->status = REF_STATUS_NONE;
			return -1;
		}
	}
	if (args->stateless_rpc && !args->dry_run)
		packet_flush(out);

	if (expect_status_report)
		ret = receive_status(in, remote_refs);
	else
		ret = 0;
	if (args->stateless_rpc)
		packet_flush(out);

	if (ret < 0)
		return ret;
	for (ref = remote_refs; ref; ref = ref->next) {
		switch (ref->status) {
		case REF_STATUS_NONE:
		case REF_STATUS_UPTODATE:
		case REF_STATUS_OK:
			break;
		default:
			return -1;
		}
	}
	return 0;
}

static void verify_remote_names(int nr_heads, const char **heads)
{
	int i;

	for (i = 0; i < nr_heads; i++) {
		const char *local = heads[i];
		const char *remote = strrchr(heads[i], ':');

		if (*local == '+')
			local++;

		/* A matching refspec is okay.  */
		if (remote == local && remote[1] == '\0')
			continue;

		remote = remote ? (remote + 1) : local;
		switch (check_ref_format(remote)) {
		case 0: /* ok */
		case CHECK_REF_FORMAT_ONELEVEL:
			/* ok but a single level -- that is fine for
			 * a match pattern.
			 */
		case CHECK_REF_FORMAT_WILDCARD:
			/* ok but ends with a pattern-match character */
			continue;
		}
		die("remote part of refspec is not a valid name in %s",
		    heads[i]);
	}
}

int cmd_send_pack(int argc, const char **argv, const char *prefix)
{
	int i, nr_refspecs = 0;
	const char **refspecs = NULL;
	const char *remote_name = NULL;
	struct remote *remote = NULL;
	const char *dest = NULL;
	int fd[2];
	struct child_process *conn;
	struct extra_have_objects extra_have;
	struct ref *remote_refs, *local_refs;
	int ret;
	int helper_status = 0;
	int send_all = 0;
	const char *receivepack = "git-receive-pack";
	int flags;

	argv++;
	for (i = 1; i < argc; i++, argv++) {
		const char *arg = *argv;

		if (*arg == '-') {
			if (!prefixcmp(arg, "--receive-pack=")) {
				receivepack = arg + 15;
				continue;
			}
			if (!prefixcmp(arg, "--exec=")) {
				receivepack = arg + 7;
				continue;
			}
			if (!prefixcmp(arg, "--remote=")) {
				remote_name = arg + 9;
				continue;
			}
			if (!strcmp(arg, "--all")) {
				send_all = 1;
				continue;
			}
			if (!strcmp(arg, "--dry-run")) {
				args.dry_run = 1;
				continue;
			}
			if (!strcmp(arg, "--mirror")) {
				args.send_mirror = 1;
				continue;
			}
			if (!strcmp(arg, "--force")) {
				args.force_update = 1;
				continue;
			}
			if (!strcmp(arg, "--verbose")) {
				args.verbose = 1;
				continue;
			}
			if (!strcmp(arg, "--thin")) {
				args.use_thin_pack = 1;
				continue;
			}
			if (!strcmp(arg, "--stateless-rpc")) {
				args.stateless_rpc = 1;
				continue;
			}
			if (!strcmp(arg, "--helper-status")) {
				helper_status = 1;
				continue;
			}
			usage(send_pack_usage);
		}
		if (!dest) {
			dest = arg;
			continue;
		}
		refspecs = (const char **) argv;
		nr_refspecs = argc - i;
		break;
	}
	if (!dest)
		usage(send_pack_usage);
	/*
	 * --all and --mirror are incompatible; neither makes sense
	 * with any refspecs.
	 */
	if ((refspecs && (send_all || args.send_mirror)) ||
	    (send_all && args.send_mirror))
		usage(send_pack_usage);

	if (remote_name) {
		remote = remote_get(remote_name);
		if (!remote_has_url(remote, dest)) {
			die("Destination %s is not a uri for %s",
			    dest, remote_name);
		}
	}

	if (args.stateless_rpc) {
		conn = NULL;
		fd[0] = 0;
		fd[1] = 1;
	} else {
		conn = git_connect(fd, dest, receivepack,
			args.verbose ? CONNECT_VERBOSE : 0);
	}

	memset(&extra_have, 0, sizeof(extra_have));

	get_remote_heads(fd[0], &remote_refs, 0, NULL, REF_NORMAL,
			 &extra_have);

	verify_remote_names(nr_refspecs, refspecs);

	local_refs = get_local_heads();

	flags = MATCH_REFS_NONE;

	if (send_all)
		flags |= MATCH_REFS_ALL;
	if (args.send_mirror)
		flags |= MATCH_REFS_MIRROR;

	/* match them up */
	if (match_refs(local_refs, &remote_refs, nr_refspecs, refspecs, flags))
		return -1;

	set_ref_status_for_push(remote_refs, args.send_mirror,
		args.force_update);

	ret = send_pack(&args, fd, conn, remote_refs, &extra_have);

	if (helper_status)
		print_helper_status(remote_refs);

	close(fd[1]);
	close(fd[0]);

	ret |= finish_connect(conn);

	if (!helper_status)
		print_push_status(dest, remote_refs);

	if (!args.dry_run && remote) {
		struct ref *ref;
		for (ref = remote_refs; ref; ref = ref->next)
			update_tracking_ref(remote, ref);
	}

	if (!ret && !refs_pushed(remote_refs))
		fprintf(stderr, "Everything up-to-date\n");

	return ret;
}
