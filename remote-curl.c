#include "cache.h"
#include "remote.h"
#include "strbuf.h"
#include "walker.h"
#include "http.h"
#include "exec_cmd.h"
#include "run-command.h"
#include "pkt-line.h"
#include "sideband.h"
#include "argv-array.h"

static struct remote *remote;
static const char *url; /* always ends with a trailing slash */

struct options {
	int verbosity;
	unsigned long depth;
	unsigned progress : 1,
		check_self_contained_and_connected : 1,
		followtags : 1,
		dry_run : 1,
		thin : 1;
};
static struct options options;

static int set_option(const char *name, const char *value)
{
	if (!strcmp(name, "verbosity")) {
		char *end;
		int v = strtol(value, &end, 10);
		if (value == end || *end)
			return -1;
		options.verbosity = v;
		return 0;
	}
	else if (!strcmp(name, "progress")) {
		if (!strcmp(value, "true"))
			options.progress = 1;
		else if (!strcmp(value, "false"))
			options.progress = 0;
		else
			return -1;
		return 0;
	}
	else if (!strcmp(name, "depth")) {
		char *end;
		unsigned long v = strtoul(value, &end, 10);
		if (value == end || *end)
			return -1;
		options.depth = v;
		return 0;
	}
	else if (!strcmp(name, "followtags")) {
		if (!strcmp(value, "true"))
			options.followtags = 1;
		else if (!strcmp(value, "false"))
			options.followtags = 0;
		else
			return -1;
		return 0;
	}
	else if (!strcmp(name, "dry-run")) {
		if (!strcmp(value, "true"))
			options.dry_run = 1;
		else if (!strcmp(value, "false"))
			options.dry_run = 0;
		else
			return -1;
		return 0;
	}
	else if (!strcmp(name, "check-connectivity")) {
		if (!strcmp(value, "true"))
			options.check_self_contained_and_connected = 1;
		else if (!strcmp(value, "false"))
			options.check_self_contained_and_connected = 0;
		else
			return -1;
		return 0;
	}
	else {
		return 1 /* unsupported */;
	}
}

struct discovery {
	const char *service;
	char *buf_alloc;
	char *buf;
	size_t len;
	struct ref *refs;
	unsigned proto_git : 1;
};
static struct discovery *last_discovery;

static struct ref *parse_git_refs(struct discovery *heads, int for_push)
{
	struct ref *list = NULL;
	get_remote_heads(-1, heads->buf, heads->len, &list,
			 for_push ? REF_NORMAL : 0, NULL);
	return list;
}

static struct ref *parse_info_refs(struct discovery *heads)
{
	char *data, *start, *mid;
	char *ref_name;
	int i = 0;

	struct ref *refs = NULL;
	struct ref *ref = NULL;
	struct ref *last_ref = NULL;

	data = heads->buf;
	start = NULL;
	mid = data;
	while (i < heads->len) {
		if (!start) {
			start = &data[i];
		}
		if (data[i] == '\t')
			mid = &data[i];
		if (data[i] == '\n') {
			if (mid - start != 40)
				die("%sinfo/refs not valid: is this a git repository?", url);
			data[i] = 0;
			ref_name = mid + 1;
			ref = xmalloc(sizeof(struct ref) +
				      strlen(ref_name) + 1);
			memset(ref, 0, sizeof(struct ref));
			strcpy(ref->name, ref_name);
			get_sha1_hex(start, ref->old_sha1);
			if (!refs)
				refs = ref;
			if (last_ref)
				last_ref->next = ref;
			last_ref = ref;
			start = NULL;
		}
		i++;
	}

	ref = alloc_ref("HEAD");
	if (!http_fetch_ref(url, ref) &&
	    !resolve_remote_symref(ref, refs)) {
		ref->next = refs;
		refs = ref;
	} else {
		free(ref);
	}

	return refs;
}

static void free_discovery(struct discovery *d)
{
	if (d) {
		if (d == last_discovery)
			last_discovery = NULL;
		free(d->buf_alloc);
		free_refs(d->refs);
		free(d);
	}
}

static int show_http_message(struct strbuf *type, struct strbuf *msg)
{
	const char *p, *eol;

	/*
	 * We only show text/plain parts, as other types are likely
	 * to be ugly to look at on the user's terminal.
	 *
	 * TODO should handle "; charset=XXX", and re-encode into
	 * logoutputencoding
	 */
	if (strcasecmp(type->buf, "text/plain"))
		return -1;

	strbuf_trim(msg);
	if (!msg->len)
		return -1;

	p = msg->buf;
	do {
		eol = strchrnul(p, '\n');
		fprintf(stderr, "remote: %.*s\n", (int)(eol - p), p);
		p = eol + 1;
	} while(*eol);
	return 0;
}

static struct discovery* discover_refs(const char *service, int for_push)
{
	struct strbuf exp = STRBUF_INIT;
	struct strbuf type = STRBUF_INIT;
	struct strbuf buffer = STRBUF_INIT;
	struct discovery *last = last_discovery;
	char *refs_url;
	int http_ret, maybe_smart = 0;

	if (last && !strcmp(service, last->service))
		return last;
	free_discovery(last);

	strbuf_addf(&buffer, "%sinfo/refs", url);
	if ((!prefixcmp(url, "http://") || !prefixcmp(url, "https://")) &&
	     git_env_bool("GIT_SMART_HTTP", 1)) {
		maybe_smart = 1;
		if (!strchr(url, '?'))
			strbuf_addch(&buffer, '?');
		else
			strbuf_addch(&buffer, '&');
		strbuf_addf(&buffer, "service=%s", service);
	}
	refs_url = strbuf_detach(&buffer, NULL);

	http_ret = http_get_strbuf(refs_url, &type, &buffer,
				   HTTP_NO_CACHE | HTTP_KEEP_ERROR);
	switch (http_ret) {
	case HTTP_OK:
		break;
	case HTTP_MISSING_TARGET:
		show_http_message(&type, &buffer);
		die("repository '%s' not found", url);
	case HTTP_NOAUTH:
		show_http_message(&type, &buffer);
		die("Authentication failed for '%s'", url);
	default:
		show_http_message(&type, &buffer);
		die("unable to access '%s': %s", url, curl_errorstr);
	}

	last= xcalloc(1, sizeof(*last_discovery));
	last->service = service;
	last->buf_alloc = strbuf_detach(&buffer, &last->len);
	last->buf = last->buf_alloc;

	strbuf_addf(&exp, "application/x-%s-advertisement", service);
	if (maybe_smart &&
	    (5 <= last->len && last->buf[4] == '#') &&
	    !strbuf_cmp(&exp, &type)) {
		char *line;

		/*
		 * smart HTTP response; validate that the service
		 * pkt-line matches our request.
		 */
		line = packet_read_line_buf(&last->buf, &last->len, NULL);

		strbuf_reset(&exp);
		strbuf_addf(&exp, "# service=%s", service);
		if (strcmp(line, exp.buf))
			die("invalid server response; got '%s'", line);
		strbuf_release(&exp);

		/* The header can include additional metadata lines, up
		 * until a packet flush marker.  Ignore these now, but
		 * in the future we might start to scan them.
		 */
		while (packet_read_line_buf(&last->buf, &last->len, NULL))
			;

		last->proto_git = 1;
	}

	if (last->proto_git)
		last->refs = parse_git_refs(last, for_push);
	else
		last->refs = parse_info_refs(last);

	free(refs_url);
	strbuf_release(&exp);
	strbuf_release(&type);
	strbuf_release(&buffer);
	last_discovery = last;
	return last;
}

static struct ref *get_refs(int for_push)
{
	struct discovery *heads;

	if (for_push)
		heads = discover_refs("git-receive-pack", for_push);
	else
		heads = discover_refs("git-upload-pack", for_push);

	return heads->refs;
}

static void output_refs(struct ref *refs)
{
	struct ref *posn;
	for (posn = refs; posn; posn = posn->next) {
		if (posn->symref)
			printf("@%s %s\n", posn->symref, posn->name);
		else
			printf("%s %s\n", sha1_to_hex(posn->old_sha1), posn->name);
	}
	printf("\n");
	fflush(stdout);
}

struct rpc_state {
	const char *service_name;
	const char **argv;
	struct strbuf *stdin_preamble;
	char *service_url;
	char *hdr_content_type;
	char *hdr_accept;
	char *buf;
	size_t alloc;
	size_t len;
	size_t pos;
	int in;
	int out;
	struct strbuf result;
	unsigned gzip_request : 1;
	unsigned initial_buffer : 1;
};

static size_t rpc_out(void *ptr, size_t eltsize,
		size_t nmemb, void *buffer_)
{
	size_t max = eltsize * nmemb;
	struct rpc_state *rpc = buffer_;
	size_t avail = rpc->len - rpc->pos;

	if (!avail) {
		rpc->initial_buffer = 0;
		avail = packet_read(rpc->out, NULL, NULL, rpc->buf, rpc->alloc, 0);
		if (!avail)
			return 0;
		rpc->pos = 0;
		rpc->len = avail;
	}

	if (max < avail)
		avail = max;
	memcpy(ptr, rpc->buf + rpc->pos, avail);
	rpc->pos += avail;
	return avail;
}

#ifndef NO_CURL_IOCTL
static curlioerr rpc_ioctl(CURL *handle, int cmd, void *clientp)
{
	struct rpc_state *rpc = clientp;

	switch (cmd) {
	case CURLIOCMD_NOP:
		return CURLIOE_OK;

	case CURLIOCMD_RESTARTREAD:
		if (rpc->initial_buffer) {
			rpc->pos = 0;
			return CURLIOE_OK;
		}
		fprintf(stderr, "Unable to rewind rpc post data - try increasing http.postBuffer\n");
		return CURLIOE_FAILRESTART;

	default:
		return CURLIOE_UNKNOWNCMD;
	}
}
#endif

static size_t rpc_in(char *ptr, size_t eltsize,
		size_t nmemb, void *buffer_)
{
	size_t size = eltsize * nmemb;
	struct rpc_state *rpc = buffer_;
	write_or_die(rpc->in, ptr, size);
	return size;
}

static int run_slot(struct active_request_slot *slot)
{
	int err;
	struct slot_results results;

	slot->results = &results;
	slot->curl_result = curl_easy_perform(slot->curl);
	finish_active_slot(slot);

	err = handle_curl_result(&results);
	if (err != HTTP_OK && err != HTTP_REAUTH) {
		error("RPC failed; result=%d, HTTP code = %ld",
		      results.curl_result, results.http_code);
	}

	return err;
}

static int probe_rpc(struct rpc_state *rpc)
{
	struct active_request_slot *slot;
	struct curl_slist *headers = NULL;
	struct strbuf buf = STRBUF_INIT;
	int err;

	slot = get_active_slot();

	headers = curl_slist_append(headers, rpc->hdr_content_type);
	headers = curl_slist_append(headers, rpc->hdr_accept);

	curl_easy_setopt(slot->curl, CURLOPT_NOBODY, 0);
	curl_easy_setopt(slot->curl, CURLOPT_POST, 1);
	curl_easy_setopt(slot->curl, CURLOPT_URL, rpc->service_url);
	curl_easy_setopt(slot->curl, CURLOPT_ENCODING, NULL);
	curl_easy_setopt(slot->curl, CURLOPT_POSTFIELDS, "0000");
	curl_easy_setopt(slot->curl, CURLOPT_POSTFIELDSIZE, 4);
	curl_easy_setopt(slot->curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION, fwrite_buffer);
	curl_easy_setopt(slot->curl, CURLOPT_FILE, &buf);

	err = run_slot(slot);

	curl_slist_free_all(headers);
	strbuf_release(&buf);
	return err;
}

static int post_rpc(struct rpc_state *rpc)
{
	struct active_request_slot *slot;
	struct curl_slist *headers = NULL;
	int use_gzip = rpc->gzip_request;
	char *gzip_body = NULL;
	size_t gzip_size = 0;
	int err, large_request = 0;

	/* Try to load the entire request, if we can fit it into the
	 * allocated buffer space we can use HTTP/1.0 and avoid the
	 * chunked encoding mess.
	 */
	while (1) {
		size_t left = rpc->alloc - rpc->len;
		char *buf = rpc->buf + rpc->len;
		int n;

		if (left < LARGE_PACKET_MAX) {
			large_request = 1;
			use_gzip = 0;
			break;
		}

		n = packet_read(rpc->out, NULL, NULL, buf, left, 0);
		if (!n)
			break;
		rpc->len += n;
	}

	if (large_request) {
		do {
			err = probe_rpc(rpc);
		} while (err == HTTP_REAUTH);
		if (err != HTTP_OK)
			return -1;
	}

	headers = curl_slist_append(headers, rpc->hdr_content_type);
	headers = curl_slist_append(headers, rpc->hdr_accept);
	headers = curl_slist_append(headers, "Expect:");

retry:
	slot = get_active_slot();

	curl_easy_setopt(slot->curl, CURLOPT_NOBODY, 0);
	curl_easy_setopt(slot->curl, CURLOPT_POST, 1);
	curl_easy_setopt(slot->curl, CURLOPT_URL, rpc->service_url);
	curl_easy_setopt(slot->curl, CURLOPT_ENCODING, "gzip");

	if (large_request) {
		/* The request body is large and the size cannot be predicted.
		 * We must use chunked encoding to send it.
		 */
		headers = curl_slist_append(headers, "Transfer-Encoding: chunked");
		rpc->initial_buffer = 1;
		curl_easy_setopt(slot->curl, CURLOPT_READFUNCTION, rpc_out);
		curl_easy_setopt(slot->curl, CURLOPT_INFILE, rpc);
#ifndef NO_CURL_IOCTL
		curl_easy_setopt(slot->curl, CURLOPT_IOCTLFUNCTION, rpc_ioctl);
		curl_easy_setopt(slot->curl, CURLOPT_IOCTLDATA, rpc);
#endif
		if (options.verbosity > 1) {
			fprintf(stderr, "POST %s (chunked)\n", rpc->service_name);
			fflush(stderr);
		}

	} else if (gzip_body) {
		/*
		 * If we are looping to retry authentication, then the previous
		 * run will have set up the headers and gzip buffer already,
		 * and we just need to send it.
		 */
		curl_easy_setopt(slot->curl, CURLOPT_POSTFIELDS, gzip_body);
		curl_easy_setopt(slot->curl, CURLOPT_POSTFIELDSIZE, gzip_size);

	} else if (use_gzip && 1024 < rpc->len) {
		/* The client backend isn't giving us compressed data so
		 * we can try to deflate it ourselves, this may save on.
		 * the transfer time.
		 */
		git_zstream stream;
		int ret;

		memset(&stream, 0, sizeof(stream));
		git_deflate_init_gzip(&stream, Z_BEST_COMPRESSION);
		gzip_size = git_deflate_bound(&stream, rpc->len);
		gzip_body = xmalloc(gzip_size);

		stream.next_in = (unsigned char *)rpc->buf;
		stream.avail_in = rpc->len;
		stream.next_out = (unsigned char *)gzip_body;
		stream.avail_out = gzip_size;

		ret = git_deflate(&stream, Z_FINISH);
		if (ret != Z_STREAM_END)
			die("cannot deflate request; zlib deflate error %d", ret);

		ret = git_deflate_end_gently(&stream);
		if (ret != Z_OK)
			die("cannot deflate request; zlib end error %d", ret);

		gzip_size = stream.total_out;

		headers = curl_slist_append(headers, "Content-Encoding: gzip");
		curl_easy_setopt(slot->curl, CURLOPT_POSTFIELDS, gzip_body);
		curl_easy_setopt(slot->curl, CURLOPT_POSTFIELDSIZE, gzip_size);

		if (options.verbosity > 1) {
			fprintf(stderr, "POST %s (gzip %lu to %lu bytes)\n",
				rpc->service_name,
				(unsigned long)rpc->len, (unsigned long)gzip_size);
			fflush(stderr);
		}
	} else {
		/* We know the complete request size in advance, use the
		 * more normal Content-Length approach.
		 */
		curl_easy_setopt(slot->curl, CURLOPT_POSTFIELDS, rpc->buf);
		curl_easy_setopt(slot->curl, CURLOPT_POSTFIELDSIZE, rpc->len);
		if (options.verbosity > 1) {
			fprintf(stderr, "POST %s (%lu bytes)\n",
				rpc->service_name, (unsigned long)rpc->len);
			fflush(stderr);
		}
	}

	curl_easy_setopt(slot->curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION, rpc_in);
	curl_easy_setopt(slot->curl, CURLOPT_FILE, rpc);

	err = run_slot(slot);
	if (err == HTTP_REAUTH && !large_request)
		goto retry;
	if (err != HTTP_OK)
		err = -1;

	curl_slist_free_all(headers);
	free(gzip_body);
	return err;
}

static int rpc_service(struct rpc_state *rpc, struct discovery *heads)
{
	const char *svc = rpc->service_name;
	struct strbuf buf = STRBUF_INIT;
	struct strbuf *preamble = rpc->stdin_preamble;
	struct child_process client;
	int err = 0;

	memset(&client, 0, sizeof(client));
	client.in = -1;
	client.out = -1;
	client.git_cmd = 1;
	client.argv = rpc->argv;
	if (start_command(&client))
		exit(1);
	if (preamble)
		write_or_die(client.in, preamble->buf, preamble->len);
	if (heads)
		write_or_die(client.in, heads->buf, heads->len);

	rpc->alloc = http_post_buffer;
	rpc->buf = xmalloc(rpc->alloc);
	rpc->in = client.in;
	rpc->out = client.out;
	strbuf_init(&rpc->result, 0);

	strbuf_addf(&buf, "%s%s", url, svc);
	rpc->service_url = strbuf_detach(&buf, NULL);

	strbuf_addf(&buf, "Content-Type: application/x-%s-request", svc);
	rpc->hdr_content_type = strbuf_detach(&buf, NULL);

	strbuf_addf(&buf, "Accept: application/x-%s-result", svc);
	rpc->hdr_accept = strbuf_detach(&buf, NULL);

	while (!err) {
		int n = packet_read(rpc->out, NULL, NULL, rpc->buf, rpc->alloc, 0);
		if (!n)
			break;
		rpc->pos = 0;
		rpc->len = n;
		err |= post_rpc(rpc);
	}

	close(client.in);
	client.in = -1;
	if (!err) {
		strbuf_read(&rpc->result, client.out, 0);
	} else {
		char buf[4096];
		for (;;)
			if (xread(client.out, buf, sizeof(buf)) <= 0)
				break;
	}

	close(client.out);
	client.out = -1;

	err |= finish_command(&client);
	free(rpc->service_url);
	free(rpc->hdr_content_type);
	free(rpc->hdr_accept);
	free(rpc->buf);
	strbuf_release(&buf);
	return err;
}

static int fetch_dumb(int nr_heads, struct ref **to_fetch)
{
	struct walker *walker;
	char **targets = xmalloc(nr_heads * sizeof(char*));
	int ret, i;

	if (options.depth)
		die("dumb http transport does not support --depth");
	for (i = 0; i < nr_heads; i++)
		targets[i] = xstrdup(sha1_to_hex(to_fetch[i]->old_sha1));

	walker = get_http_walker(url);
	walker->get_all = 1;
	walker->get_tree = 1;
	walker->get_history = 1;
	walker->get_verbosely = options.verbosity >= 3;
	walker->get_recover = 0;
	ret = walker_fetch(walker, nr_heads, targets, NULL, NULL);
	walker_free(walker);

	for (i = 0; i < nr_heads; i++)
		free(targets[i]);
	free(targets);

	return ret ? error("Fetch failed.") : 0;
}

static int fetch_git(struct discovery *heads,
	int nr_heads, struct ref **to_fetch)
{
	struct rpc_state rpc;
	struct strbuf preamble = STRBUF_INIT;
	char *depth_arg = NULL;
	int argc = 0, i, err;
	const char *argv[16];

	argv[argc++] = "fetch-pack";
	argv[argc++] = "--stateless-rpc";
	argv[argc++] = "--stdin";
	argv[argc++] = "--lock-pack";
	if (options.followtags)
		argv[argc++] = "--include-tag";
	if (options.thin)
		argv[argc++] = "--thin";
	if (options.verbosity >= 3) {
		argv[argc++] = "-v";
		argv[argc++] = "-v";
	}
	if (options.check_self_contained_and_connected)
		argv[argc++] = "--check-self-contained-and-connected";
	if (!options.progress)
		argv[argc++] = "--no-progress";
	if (options.depth) {
		struct strbuf buf = STRBUF_INIT;
		strbuf_addf(&buf, "--depth=%lu", options.depth);
		depth_arg = strbuf_detach(&buf, NULL);
		argv[argc++] = depth_arg;
	}
	argv[argc++] = url;
	argv[argc++] = NULL;

	for (i = 0; i < nr_heads; i++) {
		struct ref *ref = to_fetch[i];
		if (!ref->name || !*ref->name)
			die("cannot fetch by sha1 over smart http");
		packet_buf_write(&preamble, "%s\n", ref->name);
	}
	packet_buf_flush(&preamble);

	memset(&rpc, 0, sizeof(rpc));
	rpc.service_name = "git-upload-pack",
	rpc.argv = argv;
	rpc.stdin_preamble = &preamble;
	rpc.gzip_request = 1;

	err = rpc_service(&rpc, heads);
	if (rpc.result.len)
		write_or_die(1, rpc.result.buf, rpc.result.len);
	strbuf_release(&rpc.result);
	strbuf_release(&preamble);
	free(depth_arg);
	return err;
}

static int fetch(int nr_heads, struct ref **to_fetch)
{
	struct discovery *d = discover_refs("git-upload-pack", 0);
	if (d->proto_git)
		return fetch_git(d, nr_heads, to_fetch);
	else
		return fetch_dumb(nr_heads, to_fetch);
}

static void parse_fetch(struct strbuf *buf)
{
	struct ref **to_fetch = NULL;
	struct ref *list_head = NULL;
	struct ref **list = &list_head;
	int alloc_heads = 0, nr_heads = 0;

	do {
		if (!prefixcmp(buf->buf, "fetch ")) {
			char *p = buf->buf + strlen("fetch ");
			char *name;
			struct ref *ref;
			unsigned char old_sha1[20];

			if (strlen(p) < 40 || get_sha1_hex(p, old_sha1))
				die("protocol error: expected sha/ref, got %s'", p);
			if (p[40] == ' ')
				name = p + 41;
			else if (!p[40])
				name = "";
			else
				die("protocol error: expected sha/ref, got %s'", p);

			ref = alloc_ref(name);
			hashcpy(ref->old_sha1, old_sha1);

			*list = ref;
			list = &ref->next;

			ALLOC_GROW(to_fetch, nr_heads + 1, alloc_heads);
			to_fetch[nr_heads++] = ref;
		}
		else
			die("http transport does not support %s", buf->buf);

		strbuf_reset(buf);
		if (strbuf_getline(buf, stdin, '\n') == EOF)
			return;
		if (!*buf->buf)
			break;
	} while (1);

	if (fetch(nr_heads, to_fetch))
		exit(128); /* error already reported */
	free_refs(list_head);
	free(to_fetch);

	printf("\n");
	fflush(stdout);
	strbuf_reset(buf);
}

static int push_dav(int nr_spec, char **specs)
{
	const char **argv = xmalloc((10 + nr_spec) * sizeof(char*));
	int argc = 0, i;

	argv[argc++] = "http-push";
	argv[argc++] = "--helper-status";
	if (options.dry_run)
		argv[argc++] = "--dry-run";
	if (options.verbosity > 1)
		argv[argc++] = "--verbose";
	argv[argc++] = url;
	for (i = 0; i < nr_spec; i++)
		argv[argc++] = specs[i];
	argv[argc++] = NULL;

	if (run_command_v_opt(argv, RUN_GIT_CMD))
		die("git-%s failed", argv[0]);
	free(argv);
	return 0;
}

static int push_git(struct discovery *heads, int nr_spec, char **specs)
{
	struct rpc_state rpc;
	int i, err;
	struct argv_array args;

	argv_array_init(&args);
	argv_array_pushl(&args, "send-pack", "--stateless-rpc", "--helper-status",
			 NULL);

	if (options.thin)
		argv_array_push(&args, "--thin");
	if (options.dry_run)
		argv_array_push(&args, "--dry-run");
	if (options.verbosity == 0)
		argv_array_push(&args, "--quiet");
	else if (options.verbosity > 1)
		argv_array_push(&args, "--verbose");
	argv_array_push(&args, options.progress ? "--progress" : "--no-progress");
	argv_array_push(&args, url);
	for (i = 0; i < nr_spec; i++)
		argv_array_push(&args, specs[i]);

	memset(&rpc, 0, sizeof(rpc));
	rpc.service_name = "git-receive-pack",
	rpc.argv = args.argv;

	err = rpc_service(&rpc, heads);
	if (rpc.result.len)
		write_or_die(1, rpc.result.buf, rpc.result.len);
	strbuf_release(&rpc.result);
	argv_array_clear(&args);
	return err;
}

static int push(int nr_spec, char **specs)
{
	struct discovery *heads = discover_refs("git-receive-pack", 1);
	int ret;

	if (heads->proto_git)
		ret = push_git(heads, nr_spec, specs);
	else
		ret = push_dav(nr_spec, specs);
	free_discovery(heads);
	return ret;
}

static void parse_push(struct strbuf *buf)
{
	char **specs = NULL;
	int alloc_spec = 0, nr_spec = 0, i, ret;

	do {
		if (!prefixcmp(buf->buf, "push ")) {
			ALLOC_GROW(specs, nr_spec + 1, alloc_spec);
			specs[nr_spec++] = xstrdup(buf->buf + 5);
		}
		else
			die("http transport does not support %s", buf->buf);

		strbuf_reset(buf);
		if (strbuf_getline(buf, stdin, '\n') == EOF)
			goto free_specs;
		if (!*buf->buf)
			break;
	} while (1);

	ret = push(nr_spec, specs);
	printf("\n");
	fflush(stdout);

	if (ret)
		exit(128); /* error already reported */

 free_specs:
	for (i = 0; i < nr_spec; i++)
		free(specs[i]);
	free(specs);
}

int main(int argc, const char **argv)
{
	struct strbuf buf = STRBUF_INIT;
	int nongit;

	git_extract_argv0_path(argv[0]);
	setup_git_directory_gently(&nongit);
	if (argc < 2) {
		fprintf(stderr, "Remote needed\n");
		return 1;
	}

	options.verbosity = 1;
	options.progress = !!isatty(2);
	options.thin = 1;

	remote = remote_get(argv[1]);

	if (argc > 2) {
		end_url_with_slash(&buf, argv[2]);
	} else {
		end_url_with_slash(&buf, remote->url[0]);
	}

	url = strbuf_detach(&buf, NULL);

	http_init(remote, url, 0);

	do {
		if (strbuf_getline(&buf, stdin, '\n') == EOF) {
			if (ferror(stdin))
				fprintf(stderr, "Error reading command stream\n");
			else
				fprintf(stderr, "Unexpected end of command stream\n");
			return 1;
		}
		if (buf.len == 0)
			break;
		if (!prefixcmp(buf.buf, "fetch ")) {
			if (nongit)
				die("Fetch attempted without a local repo");
			parse_fetch(&buf);

		} else if (!strcmp(buf.buf, "list") || !prefixcmp(buf.buf, "list ")) {
			int for_push = !!strstr(buf.buf + 4, "for-push");
			output_refs(get_refs(for_push));

		} else if (!prefixcmp(buf.buf, "push ")) {
			parse_push(&buf);

		} else if (!prefixcmp(buf.buf, "option ")) {
			char *name = buf.buf + strlen("option ");
			char *value = strchr(name, ' ');
			int result;

			if (value)
				*value++ = '\0';
			else
				value = "true";

			result = set_option(name, value);
			if (!result)
				printf("ok\n");
			else if (result < 0)
				printf("error invalid value\n");
			else
				printf("unsupported\n");
			fflush(stdout);

		} else if (!strcmp(buf.buf, "capabilities")) {
			printf("fetch\n");
			printf("option\n");
			printf("push\n");
			printf("check-connectivity\n");
			printf("\n");
			fflush(stdout);
		} else {
			fprintf(stderr, "Unknown command '%s'\n", buf.buf);
			return 1;
		}
		strbuf_reset(&buf);
	} while (1);

	http_cleanup();

	return 0;
}
