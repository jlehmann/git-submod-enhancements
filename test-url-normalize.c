#ifdef NO_CURL

int main()
{
	return 125;
}

#else /* !NO_CURL */

#include "http.c"

static int run_http_options(const char *file,
			    const char *opt,
			    const struct url_info *info)
{
	struct strbuf opt_lc;
	size_t i, len;

	if (git_config_with_options(http_options, (void *)info, file, NULL, 0))
		return 1;

	len = strlen(opt);
	strbuf_init(&opt_lc, len);
	for (i = 0; i < len; ++i) {
		strbuf_addch(&opt_lc, tolower(opt[i]));
	}

	if (!strcmp("sslverify", opt_lc.buf))
		printf("%s\n", curl_ssl_verify ? "true" : "false");
	else if (!strcmp("sslcert", opt_lc.buf))
		printf("%s\n", ssl_cert);
#if LIBCURL_VERSION_NUM >= 0x070903
	else if (!strcmp("sslkey", opt_lc.buf))
		printf("%s\n", ssl_key);
#endif
#if LIBCURL_VERSION_NUM >= 0x070908
	else if (!strcmp("sslcapath", opt_lc.buf))
		printf("%s\n", ssl_capath);
#endif
	else if (!strcmp("sslcainfo", opt_lc.buf))
		printf("%s\n", ssl_cainfo);
	else if (!strcmp("sslcertpasswordprotected", opt_lc.buf))
		printf("%s\n", ssl_cert_password_required ? "true" : "false");
	else if (!strcmp("ssltry", opt_lc.buf))
		printf("%s\n", curl_ssl_try ? "true" : "false");
	else if (!strcmp("minsessions", opt_lc.buf))
		printf("%d\n", min_curl_sessions);
	else if (!strcmp("maxrequests", opt_lc.buf))
		printf("%d\n", max_requests);
	else if (!strcmp("lowspeedlimit", opt_lc.buf))
		printf("%ld\n", curl_low_speed_limit);
	else if (!strcmp("lowspeedtime", opt_lc.buf))
		printf("%ld\n", curl_low_speed_time);
	else if (!strcmp("noepsv", opt_lc.buf))
		printf("%s\n", curl_ftp_no_epsv ? "true" : "false");
	else if (!strcmp("proxy", opt_lc.buf))
		printf("%s\n", curl_http_proxy);
	else if (!strcmp("cookiefile", opt_lc.buf))
		printf("%s\n", curl_cookie_file);
	else if (!strcmp("postbuffer", opt_lc.buf))
		printf("%u\n", (unsigned)http_post_buffer);
	else if (!strcmp("useragent", opt_lc.buf))
		printf("%s\n", user_agent);

	return 0;
}

#define url_normalize(u,i) http_options_url_normalize(u,i)

int main(int argc, char **argv)
{
	const char *usage = "test-url-normalize [-p | -l] <url1> | <url1> <url2>"
		" | -c file option <url1>";
	char *url1, *url2;
	int opt_p = 0, opt_l = 0, opt_c = 0;
	char *file = NULL, *optname = NULL;

	/*
	 * For one url, succeed if url_normalize succeeds on it, fail otherwise.
	 * For two urls, succeed only if url_normalize succeeds on both and
	 * the results compare equal with strcmp.  If -p is given (one url only)
	 * and url_normalize succeeds, print the result followed by "\n".  If
	 * -l is given (one url only) and url_normalize succeeds, print the
	 * returned length in decimal followed by "\n".
	 * If -c is given, call git_config_with_options using the specified file
	 * and http_options and passing the normalized value of the url.  Then
	 * print the value of 'option' afterwards.  'option' must be one of the
	 * valid 'http.*' options.
	 */

	if (argc > 1 && !strcmp(argv[1], "-p")) {
		opt_p = 1;
		argc--;
		argv++;
	} else if (argc > 1 && !strcmp(argv[1], "-l")) {
		opt_l = 1;
		argc--;
		argv++;
	} else if (argc > 3 && !strcmp(argv[1], "-c")) {
		opt_c = 1;
		file = argv[2];
		optname = argv[3];
		argc -= 3;
		argv += 3;
	}

	if (argc < 2 || argc > 3)
		die(usage);

	if (argc == 2) {
		struct url_info info;
		url1 = url_normalize(argv[1], &info);
		if (!url1)
			return 1;
		if (opt_p)
			printf("%s\n", url1);
		if (opt_l)
			printf("%u\n", (unsigned)info.url_len);
		if (opt_c)
			return run_http_options(file, optname, &info);
		return 0;
	}

	if (opt_p || opt_l || opt_c)
		die(usage);

	url1 = url_normalize(argv[1], NULL);
	url2 = url_normalize(argv[2], NULL);
	return (url1 && url2 && !strcmp(url1, url2)) ? 0 : 1;
}

#endif /* !NO_CURL */
