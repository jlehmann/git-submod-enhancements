#include "http.h"
#include "pack.h"
#include "sideband.h"
#include "run-command.h"
#include "url.h"
#include "credential.h"
#include "version.h"
#include "pkt-line.h"

int active_requests;
int http_is_verbose;
size_t http_post_buffer = 16 * LARGE_PACKET_MAX;

#if LIBCURL_VERSION_NUM >= 0x070a06
#define LIBCURL_CAN_HANDLE_AUTH_ANY
#endif

static int min_curl_sessions = 1;
static int curl_session_count;
#ifdef USE_CURL_MULTI
static int max_requests = -1;
static CURLM *curlm;
#endif
#ifndef NO_CURL_EASY_DUPHANDLE
static CURL *curl_default;
#endif

#define PREV_BUF_SIZE 4096
#define RANGE_HEADER_SIZE 30

char curl_errorstr[CURL_ERROR_SIZE];

enum http_option_type {
	OPT_POST_BUFFER,
	OPT_MIN_SESSIONS,
	OPT_SSL_VERIFY,
	OPT_SSL_TRY,
	OPT_SSL_CERT,
	OPT_SSL_CAINFO,
	OPT_LOW_SPEED,
	OPT_LOW_TIME,
	OPT_NO_EPSV,
	OPT_HTTP_PROXY,
	OPT_COOKIE_FILE,
	OPT_USER_AGENT,
	OPT_PASSWD_REQ,
#ifdef USE_CURL_MULTI
	OPT_MAX_REQUESTS,
#endif
#if LIBCURL_VERSION_NUM >= 0x070903
	OPT_SSL_KEY,
#endif
#if LIBCURL_VERSION_NUM >= 0x070908
	OPT_SSL_CAPATH,
#endif
	OPT_MAX
};

struct url_info {
	char *url;		/* normalized url on success, must be freed, otherwise NULL */
	const char *err;	/* if !url, a brief reason for the failure, otherwise NULL */

	/* the rest of the fields are only set if url != NULL */

	size_t url_len;		/* total length of url (which is now normalized) */
	size_t scheme_len;	/* length of scheme name (excluding final :) */
	size_t user_off;	/* offset into url to start of user name (0 => none) */
	size_t user_len;	/* length of user name; if user_off != 0 but
				   user_len == 0, an empty user name was given */
	size_t passwd_off;	/* offset into url to start of passwd (0 => none) */
	size_t passwd_len;	/* length of passwd; if passwd_off != 0 but
				   passwd_len == 0, an empty passwd was given */
	size_t host_off;	/* offset into url to start of host name (0 => none) */
	size_t host_len;	/* length of host name; this INCLUDES any ':portnum';
				 * file urls may have host_len == 0 */
	size_t port_len;	/* if a portnum is present (port_len != 0), it has
				 * this length (excluding the leading ':') at the
				 * end of the host name (always 0 for file urls) */
	size_t path_off;	/* offset into url to the start of the url path;
				 * this will always point to a '/' character
				 * after the url has been normalized */
	size_t path_len;	/* length of path portion excluding any trailing
				 * '?...' and '#...' portion; will always be >= 1 */
};

static size_t http_option_max_matched_len[OPT_MAX];
static int http_option_user_matched[OPT_MAX];

static int curl_ssl_verify = -1;
static int curl_ssl_try;
static const char *ssl_cert;
#if LIBCURL_VERSION_NUM >= 0x070903
static const char *ssl_key;
#endif
#if LIBCURL_VERSION_NUM >= 0x070908
static const char *ssl_capath;
#endif
static const char *ssl_cainfo;
static long curl_low_speed_limit = -1;
static long curl_low_speed_time = -1;
static int curl_ftp_no_epsv;
static const char *curl_http_proxy;
static const char *curl_cookie_file;
static struct credential http_auth = CREDENTIAL_INIT;
static int http_proactive_auth;
static const char *user_agent;

#if LIBCURL_VERSION_NUM >= 0x071700
/* Use CURLOPT_KEYPASSWD as is */
#elif LIBCURL_VERSION_NUM >= 0x070903
#define CURLOPT_KEYPASSWD CURLOPT_SSLKEYPASSWD
#else
#define CURLOPT_KEYPASSWD CURLOPT_SSLCERTPASSWD
#endif

static struct credential cert_auth = CREDENTIAL_INIT;
static int ssl_cert_password_required;

static struct curl_slist *pragma_header;
static struct curl_slist *no_pragma_header;

static struct active_request_slot *active_queue_head;

size_t fread_buffer(char *ptr, size_t eltsize, size_t nmemb, void *buffer_)
{
	size_t size = eltsize * nmemb;
	struct buffer *buffer = buffer_;

	if (size > buffer->buf.len - buffer->posn)
		size = buffer->buf.len - buffer->posn;
	memcpy(ptr, buffer->buf.buf + buffer->posn, size);
	buffer->posn += size;

	return size;
}

#ifndef NO_CURL_IOCTL
curlioerr ioctl_buffer(CURL *handle, int cmd, void *clientp)
{
	struct buffer *buffer = clientp;

	switch (cmd) {
	case CURLIOCMD_NOP:
		return CURLIOE_OK;

	case CURLIOCMD_RESTARTREAD:
		buffer->posn = 0;
		return CURLIOE_OK;

	default:
		return CURLIOE_UNKNOWNCMD;
	}
}
#endif

size_t fwrite_buffer(char *ptr, size_t eltsize, size_t nmemb, void *buffer_)
{
	size_t size = eltsize * nmemb;
	struct strbuf *buffer = buffer_;

	strbuf_add(buffer, ptr, size);
	return size;
}

size_t fwrite_null(char *ptr, size_t eltsize, size_t nmemb, void *strbuf)
{
	return eltsize * nmemb;
}

#ifdef USE_CURL_MULTI
static void process_curl_messages(void)
{
	int num_messages;
	struct active_request_slot *slot;
	CURLMsg *curl_message = curl_multi_info_read(curlm, &num_messages);

	while (curl_message != NULL) {
		if (curl_message->msg == CURLMSG_DONE) {
			int curl_result = curl_message->data.result;
			slot = active_queue_head;
			while (slot != NULL &&
			       slot->curl != curl_message->easy_handle)
				slot = slot->next;
			if (slot != NULL) {
				curl_multi_remove_handle(curlm, slot->curl);
				slot->curl_result = curl_result;
				finish_active_slot(slot);
			} else {
				fprintf(stderr, "Received DONE message for unknown request!\n");
			}
		} else {
			fprintf(stderr, "Unknown CURL message received: %d\n",
				(int)curl_message->msg);
		}
		curl_message = curl_multi_info_read(curlm, &num_messages);
	}
}
#endif

#define URL_ALPHA "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
#define URL_DIGIT "0123456789"
#define URL_ALPHADIGIT URL_ALPHA URL_DIGIT
#define URL_SCHEME_CHARS URL_ALPHADIGIT "+.-"
#define URL_HOST_CHARS URL_ALPHADIGIT ".-[:]" /* IPv6 literals need [:] */
#define URL_UNSAFE_CHARS " <>\"%{}|\\^`" /* plus 0x00-0x1F,0x7F-0xFF */
#define URL_GEN_RESERVED ":/?#[]@"
#define URL_SUB_RESERVED "!$&'()*+,;="
#define URL_RESERVED URL_GEN_RESERVED URL_SUB_RESERVED /* only allowed delims */

static int append_normalized_escapes(struct strbuf *buf,
				     const char *from,
				     size_t from_len,
				     const char *esc_extra,
				     const char *esc_ok)
{
	/*
	 * Append to strbuf 'buf' characters from string 'from' with length
	 * 'from_len' while unescaping characters that do not need to be escaped
	 * and escaping characters that do.  The set of characters to escape
	 * (the complement of which is unescaped) starts out as the RFC 3986
	 * unsafe characters (0x00-0x1F,0x7F-0xFF," <>\"#%{}|\\^`").  If
	 * 'esc_extra' is not NULL, those additional characters will also always
	 * be escaped.  If 'esc_ok' is not NULL, those characters will be left
	 * escaped if found that way, but will not be unescaped otherwise (used
	 * for delimiters).  If a %-escape sequence is encountered that is not
	 * followed by 2 hexadecimal digits, the sequence is invalid and
	 * false (0) will be returned.  Otherwise true (1) will be returned for
	 * success.
	 *
	 * Note that all %-escape sequences will be normalized to UPPERCASE
	 * as indicated in RFC 3986.  Unless included in esc_extra or esc_ok
	 * alphanumerics and "-._~" will always be unescaped as per RFC 3986.
	 */

	while (from_len) {
		int ch = *from++;
		int was_esc = 0;

		from_len--;
		if (ch == '%') {
			if (from_len < 2 ||
			    !isxdigit((unsigned char)from[0]) ||
			    !isxdigit((unsigned char)from[1]))
				return 0;
			ch = hexval_table[(unsigned char)*from++] << 4;
			ch |= hexval_table[(unsigned char)*from++];
			from_len -= 2;
			was_esc = 1;
		}
		if ((unsigned char)ch <= 0x1F || (unsigned char)ch >= 0x7F ||
		    strchr(URL_UNSAFE_CHARS, ch) ||
		    (esc_extra && strchr(esc_extra, ch)) ||
		    (was_esc && strchr(esc_ok, ch)))
			strbuf_addf(buf, "%%%02X", (unsigned char)ch);
		else
			strbuf_addch(buf, ch);
	}

	return 1;
}

static char *http_options_url_normalize(const char *url, struct url_info *out_info)
{
	/*
	 * Normalize NUL-terminated url using the following rules:
	 *
	 * 1. Case-insensitive parts of url will be converted to lower case
	 * 2. %-encoded characters that do not need to be will be unencoded
	 * 3. Characters that are not %-encoded and must be will be encoded
	 * 4. All %-encodings will be converted to upper case hexadecimal
	 * 5. Leading 0s are removed from port numbers
	 * 6. If the default port for the scheme is given it will be removed
	 * 7. A path part (including empty) not starting with '/' has one added
	 * 8. Any dot segments (. or ..) in the path are resolved and removed
	 * 9. IPv6 host literals are allowed (but not normalized or validated)
	 *
	 * The rules are based on information in RFC 3986.
	 *
	 * Please note this function requires a full URL including a scheme
	 * and host part (except for file: URLs which may have an empty host).
	 *
	 * The return value is a newly allocated string that must be freed
	 * or NULL if the url is not valid.
	 *
	 * If out_info is non-NULL, the url and err fields therein will always
	 * be set.  If a non-NULL value is returned, it will be stored in
	 * out_info->url as well, out_info->err will be set to NULL and the
	 * other fields of *out_info will also be filled in.  If a NULL value
	 * is returned, NULL will be stored in out_info->url and out_info->err
	 * will be set to a brief, translated, error message, but no other
	 * fields will be filled in.
	 *
	 * This is NOT a URL validation function.  Full URL validation is NOT
	 * performed.  Some invalid host names are passed through this function
	 * undetected.  However, most all other problems that make a URL invalid
	 * will be detected (including a missing host for non file: URLs).
	 */

	size_t url_len = strlen(url);
	struct strbuf norm;
	size_t spanned;
	size_t scheme_len, user_off=0, user_len=0, passwd_off=0, passwd_len=0;
	size_t host_off=0, host_len=0, port_len=0, path_off, path_len, result_len;
	const char *slash_ptr, *at_ptr, *colon_ptr, *path_start;
	char *result;

	/*
	 * Copy lowercased scheme and :// suffix, %-escapes are not allowed
	 * First character of scheme must be URL_ALPHA
	 */
	spanned = strspn(url, URL_SCHEME_CHARS);
	if (!spanned || !isalpha(url[0]) || spanned + 3 > url_len ||
	    url[spanned] != ':' || url[spanned+1] != '/' || url[spanned+2] != '/') {
		if (out_info) {
			out_info->url = NULL;
			out_info->err = _("invalid URL scheme name or missing '://' suffix");
		}
		return NULL; /* Bad scheme and/or missing "://" part */
	}
	strbuf_init(&norm, url_len);
	scheme_len = spanned;
	spanned += 3;
	url_len -= spanned;
	while (spanned--)
		strbuf_addch(&norm, tolower(*url++));


	/*
	 * Copy any username:password if present normalizing %-escapes
	 */
	at_ptr = strchr(url, '@');
	slash_ptr = url + strcspn(url, "/?#");
	if (at_ptr && at_ptr < slash_ptr) {
		user_off = norm.len;
		if (at_ptr > url) {
			if (!append_normalized_escapes(&norm, url, at_ptr - url,
						       "", URL_RESERVED)) {
				if (out_info) {
					out_info->url = NULL;
					out_info->err = _("invalid %XX escape sequence");
				}
				strbuf_release(&norm);
				return NULL;
			}
			colon_ptr = strchr(norm.buf + scheme_len + 3, ':');
			if (colon_ptr) {
				passwd_off = (colon_ptr + 1) - norm.buf;
				passwd_len = norm.len - passwd_off;
				user_len = (passwd_off - 1) - (scheme_len + 3);
			} else {
				user_len = norm.len - (scheme_len + 3);
			}
		}
		strbuf_addch(&norm, '@');
		url_len -= (++at_ptr - url);
		url = at_ptr;
	}


	/*
	 * Copy the host part excluding any port part, no %-escapes allowed
	 */
	if (!url_len || strchr(":/?#", *url)) {
		/* Missing host invalid for all URL schemes except file */
		if (strncmp(norm.buf, "file:", 5)) {
			if (out_info) {
				out_info->url = NULL;
				out_info->err = _("missing host and scheme is not 'file:'");
			}
			strbuf_release(&norm);
			return NULL;
		}
	} else {
		host_off = norm.len;
	}
	colon_ptr = slash_ptr - 1;
	while (colon_ptr > url && *colon_ptr != ':' && *colon_ptr != ']')
		colon_ptr--;
	if (*colon_ptr != ':') {
		colon_ptr = slash_ptr;
	} else if (!host_off && colon_ptr < slash_ptr && colon_ptr + 1 != slash_ptr) {
		/* file: URLs may not have a port number */
		if (out_info) {
			out_info->url = NULL;
			out_info->err = _("a 'file:' URL may not have a port number");
		}
		strbuf_release(&norm);
		return NULL;
	}
	spanned = strspn(url, URL_HOST_CHARS);
	if (spanned < colon_ptr - url) {
		/* Host name has invalid characters */
		if (out_info) {
			out_info->url = NULL;
			out_info->err = _("invalid characters in host name");
		}
		strbuf_release(&norm);
		return NULL;
	}
	while (url < colon_ptr) {
		strbuf_addch(&norm, tolower(*url++));
		url_len--;
	}


	/*
	 * Check the port part and copy if not the default (after removing any
	 * leading 0s); no %-escapes allowed
	 */
	if (colon_ptr < slash_ptr) {
		/* skip the ':' and leading 0s but not the last one if all 0s */
		url++;
		url += strspn(url, "0");
		if (url == slash_ptr && url[-1] == '0')
			url--;
		if (url == slash_ptr) {
			/* Skip ":" port with no number, it's same as default */
		} else if (slash_ptr - url == 2 &&
			   !strncmp(norm.buf, "http:", 5) &&
			   !strncmp(url, "80", 2)) {
			/* Skip http :80 as it's the default */
		} else if (slash_ptr - url == 3 &&
			   !strncmp(norm.buf, "https:", 6) &&
			   !strncmp(url, "443", 3)) {
			/* Skip https :443 as it's the default */
		} else {
			/*
			 * Port number must be all digits with leading 0s removed
			 * and since all the protocols we deal with have a 16-bit
			 * port number it must also be in the range 1..65535
			 * 0 is not allowed because that means "next available"
			 * on just about every system and therefore cannot be used
			 */
			unsigned long pnum = 0;
			spanned = strspn(url, URL_DIGIT);
			if (spanned < slash_ptr - url) {
				/* port number has invalid characters */
				if (out_info) {
					out_info->url = NULL;
					out_info->err = _("invalid port number");
				}
				strbuf_release(&norm);
				return NULL;
			}
			if (slash_ptr - url <= 5)
				pnum = strtoul(url, NULL, 10);
			if (pnum == 0 || pnum > 65535) {
				/* port number not in range 1..65535 */
				if (out_info) {
					out_info->url = NULL;
					out_info->err = _("invalid port number");
				}
				strbuf_release(&norm);
				return NULL;
			}
			strbuf_addch(&norm, ':');
			strbuf_add(&norm, url, slash_ptr - url);
			port_len = slash_ptr - url;
		}
		url_len -= slash_ptr - colon_ptr;
		url = slash_ptr;
	}
	if (host_off)
		host_len = norm.len - host_off;


	/*
	 * Now copy the path resolving any . and .. segments being careful not
	 * to corrupt the URL by unescaping any delimiters, but do add an
	 * initial '/' if it's missing and do normalize any %-escape sequences.
	 */
	path_off = norm.len;
	path_start = norm.buf + path_off;
	strbuf_addch(&norm, '/');
	if (*url == '/') {
		url++;
		url_len--;
	}
	for (;;) {
		const char *seg_start = norm.buf + norm.len;
		const char *next_slash = url + strcspn(url, "/?#");
		int skip_add_slash = 0;
		/*
		 * RFC 3689 indicates that any . or .. segments should be
		 * unescaped before being checked for.
		 */
		if (!append_normalized_escapes(&norm, url, next_slash - url, "",
					       URL_RESERVED)) {
			if (out_info) {
				out_info->url = NULL;
				out_info->err = _("invalid %XX escape sequence");
			}
			strbuf_release(&norm);
			return NULL;
		}
		if (!strcmp(seg_start, ".")) {
			/* ignore a . segment; be careful not to remove initial '/' */
			if (seg_start == path_start + 1) {
				strbuf_setlen(&norm, norm.len - 1);
				skip_add_slash = 1;
			} else {
				strbuf_setlen(&norm, norm.len - 2);
			}
		} else if (!strcmp(seg_start, "..")) {
			/*
			 * ignore a .. segment and remove the previous segment;
			 * be careful not to remove initial '/' from path
			 */
			const char *prev_slash = norm.buf + norm.len - 3;
			if (prev_slash == path_start) {
				/* invalid .. because no previous segment to remove */
				if (out_info) {
					out_info->url = NULL;
					out_info->err = _("invalid '..' path segment");
				}
				strbuf_release(&norm);
				return NULL;
			}
			while (*--prev_slash != '/') {}
			if (prev_slash == path_start) {
				strbuf_setlen(&norm, prev_slash - norm.buf + 1);
				skip_add_slash = 1;
			} else {
				strbuf_setlen(&norm, prev_slash - norm.buf);
			}
		}
		url_len -= next_slash - url;
		url = next_slash;
		/* if the next char is not '/' done with the path */
		if (*url != '/')
			break;
		url++;
		url_len--;
		if (!skip_add_slash)
			strbuf_addch(&norm, '/');
	}
	path_len = norm.len - path_off;


	/*
	 * Now simply copy the rest, if any, only normalizing %-escapes and
	 * being careful not to corrupt the URL by unescaping any delimiters.
	 */
	if (*url) {
		if (!append_normalized_escapes(&norm, url, url_len, "", URL_RESERVED)) {
			if (out_info) {
				out_info->url = NULL;
				out_info->err = _("invalid %XX escape sequence");
			}
			strbuf_release(&norm);
			return NULL;
		}
	}


	result = strbuf_detach(&norm, &result_len);
	if (out_info) {
		out_info->url = result;
		out_info->err = NULL;
		out_info->url_len = result_len;
		out_info->scheme_len = scheme_len;
		out_info->user_off = user_off;
		out_info->user_len = user_len;
		out_info->passwd_off = passwd_off;
		out_info->passwd_len = passwd_len;
		out_info->host_off = host_off;
		out_info->host_len = host_len;
		out_info->port_len = port_len;
		out_info->path_off = path_off;
		out_info->path_len = path_len;
	}
	return result;
}

static size_t http_options_url_match_prefix(const char *url,
					    const char *url_prefix,
					    size_t url_prefix_len)
{
	/*
	 * url_prefix matches url if url_prefix is an exact match for url or it
	 * is a prefix of url and the match ends on a path component boundary.
	 * Both url and url_prefix are considered to have an implicit '/' on the
	 * end for matching purposes if they do not already.
	 *
	 * url must be NUL terminated.  url_prefix_len is the length of
	 * url_prefix which need not be NUL terminated.
	 *
	 * The return value is the length of the match in characters (including
	 * the final '/' even if it's implicit) or 0 for no match.
	 *
	 * Passing NULL as url and/or url_prefix will always cause 0 to be
	 * returned without causing any faults.
	 */
	if (!url || !url_prefix)
		return 0;
	if (!url_prefix_len || (url_prefix_len == 1 && *url_prefix == '/'))
		return (!*url || *url == '/') ? 1 : 0;
	if (url_prefix[url_prefix_len - 1] == '/')
		url_prefix_len--;
	if (strncmp(url, url_prefix, url_prefix_len))
		return 0;
	if ((strlen(url) == url_prefix_len) || (url[url_prefix_len] == '/'))
		return url_prefix_len + 1;
	return 0;
}

static int http_options_match_urls(const struct url_info *url,
				   const struct url_info *url_prefix,
				   int *exactusermatch)
{
	/*
	 * url_prefix matches url if the scheme, host and port of url_prefix
	 * are the same as those of url and the path portion of url_prefix
	 * is the same as the path portion of url or it is a prefix that
	 * matches at a '/' boundary.  If url_prefix contains a user name,
	 * that must also exactly match the user name in url.
	 *
	 * If the user, host, port and path match in this fashion, the returned
	 * value is the length of the path match including any implicit
	 * final '/'.  For example, "http://me@example.com/path" is matched by
	 * "http://example.com" with a path length of 1.
	 *
	 * If there is a match and exactusermatch is not NULL, then
	 * *exactusermatch will be set to true if both url and url_prefix
	 * contained a user name or false if url_prefix did not have a
	 * user name.  If there is no match *exactusermatch is left untouched.
	 */
	int usermatched = 0;
	int pathmatchlen;

	if (!url || !url_prefix || !url->url || !url_prefix->url)
		return 0;

	/* check the scheme */
	if (url_prefix->scheme_len != url->scheme_len ||
	    strncmp(url->url, url_prefix->url, url->scheme_len))
		return 0; /* schemes do not match */

	/* check the user name if url_prefix has one */
	if (url_prefix->user_off) {
		if (!url->user_off || url->user_len != url_prefix->user_len ||
		    strncmp(url->url + url->user_off,
			    url_prefix->url + url_prefix->user_off,
			    url->user_len))
			return 0; /* url_prefix has a user but it's not a match */
		usermatched = 1;
	}

	/* check the host and port */
	if (url_prefix->host_len != url->host_len ||
	    strncmp(url->url + url->host_off,
		    url_prefix->url + url_prefix->host_off, url->host_len))
		return 0; /* host names and/or ports do not match */

	/* check the path */
	pathmatchlen = http_options_url_match_prefix(
		url->url + url->path_off,
		url_prefix->url + url_prefix->path_off,
		url_prefix->url_len - url_prefix->path_off);

	if (pathmatchlen && exactusermatch)
		*exactusermatch = usermatched;
	return pathmatchlen;
}

static int match_is_ignored(size_t matchlen, int usermatch, enum http_option_type opt)
{
	/*
	 * Compare matchlen to the last matched path length of option opt and
	 * return true if matchlen is shorter than the last matched length
	 * (meaning the config setting should be ignored).  Upon seeing the
	 * _same_ key (i.e. new key has the same match length which is therefore
	 * not shorter) the new setting will override the previous setting
	 * unless the new setting did not match the user and the previous match
	 * did.  Otherwise return false and record matchlen as the current last
	 * matched path length of option opt and usermatch as the last user
	 * matching state for option opt.
	 */
	if (matchlen < http_option_max_matched_len[opt])
		return 1;
	if (matchlen > http_option_max_matched_len[opt]) {
		http_option_max_matched_len[opt] = matchlen;
		http_option_user_matched[opt] = usermatch;
		return 0;
	}
	/*
	 * If a previous match of the same length explicitly matched the user
	 * name, but the current match matched on any user, ignore it.
	 */
	if (!usermatch && http_option_user_matched[opt])
		return 1;
	http_option_user_matched[opt] = usermatch;
	return 0;
}

static int http_options(const char *var, const char *value, void *cb)
{
	const struct url_info *info = cb;
	const char *key, *dot;
	size_t matchlen = 0;
	int usermatch = 0;

	key = skip_prefix(var, "http.");
	if (!key)
		return git_default_config(var, value, cb);

	/*
	 * If the part following the leading "http." contains a '.' then check
	 * to see if the part between "http." and the last '.' is a match to
	 * url.  If it's not then ignore the setting.  Otherwise set key to
	 * point to the option which is the part after the final '.' and
	 * use key in subsequent comparisons to determine the option type.
	 */
	dot = strrchr(key, '.');
	if (dot) {
		char *config_url;
		struct url_info norm_info;
		char *norm_url;

		if (!info || !info->url)
			return 0;
		config_url = xmemdupz(key, dot - key);
		norm_url = http_options_url_normalize(config_url, &norm_info);
		free(config_url);
		if (!norm_url)
			return 0;
		matchlen = http_options_match_urls(info, &norm_info, &usermatch);
		free(norm_url);
		if (!matchlen)
			return 0;
		key = dot + 1;
	}

	if (!strcmp("sslverify", key)) {
		if (match_is_ignored(matchlen, usermatch, OPT_SSL_VERIFY))
			return 0;
		curl_ssl_verify = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp("sslcert", key)) {
		if (match_is_ignored(matchlen, usermatch, OPT_SSL_CERT))
			return 0;
		return git_config_string(&ssl_cert, var, value);
	}
#if LIBCURL_VERSION_NUM >= 0x070903
	if (!strcmp("sslkey", key)) {
		if (match_is_ignored(matchlen, usermatch, OPT_SSL_KEY))
			return 0;
		return git_config_string(&ssl_key, var, value);
	}
#endif
#if LIBCURL_VERSION_NUM >= 0x070908
	if (!strcmp("sslcapath", key)) {
		if (match_is_ignored(matchlen, usermatch, OPT_SSL_CAPATH))
			return 0;
		return git_config_string(&ssl_capath, var, value);
	}
#endif
	if (!strcmp("sslcainfo", key)) {
		if (match_is_ignored(matchlen, usermatch, OPT_SSL_CAINFO))
			return 0;
		return git_config_string(&ssl_cainfo, var, value);
	}
	if (!strcmp("sslcertpasswordprotected", key)) {
		if (match_is_ignored(matchlen, usermatch, OPT_PASSWD_REQ))
			return 0;
		ssl_cert_password_required = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp("ssltry", key)) {
		if (match_is_ignored(matchlen, usermatch, OPT_SSL_TRY))
			return 0;
		curl_ssl_try = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp("minsessions", key)) {
		if (match_is_ignored(matchlen, usermatch, OPT_MIN_SESSIONS))
			return 0;
		min_curl_sessions = git_config_int(var, value);
#ifndef USE_CURL_MULTI
		if (min_curl_sessions > 1)
			min_curl_sessions = 1;
#endif
		return 0;
	}
#ifdef USE_CURL_MULTI
	if (!strcmp("maxrequests", key)) {
		if (match_is_ignored(matchlen, usermatch, OPT_MAX_REQUESTS))
			return 0;
		max_requests = git_config_int(var, value);
		return 0;
	}
#endif
	if (!strcmp("lowspeedlimit", key)) {
		if (match_is_ignored(matchlen, usermatch, OPT_LOW_SPEED))
			return 0;
		curl_low_speed_limit = (long)git_config_int(var, value);
		return 0;
	}
	if (!strcmp("lowspeedtime", key)) {
		if (match_is_ignored(matchlen, usermatch, OPT_LOW_TIME))
			return 0;
		curl_low_speed_time = (long)git_config_int(var, value);
		return 0;
	}

	if (!strcmp("noepsv", key)) {
		if (match_is_ignored(matchlen, usermatch, OPT_NO_EPSV))
			return 0;
		curl_ftp_no_epsv = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp("proxy", key)) {
		if (match_is_ignored(matchlen, usermatch, OPT_HTTP_PROXY))
			return 0;
		return git_config_string(&curl_http_proxy, var, value);
	}

	if (!strcmp("cookiefile", key)) {
		if (match_is_ignored(matchlen, usermatch, OPT_COOKIE_FILE))
			return 0;
		return git_config_string(&curl_cookie_file, var, value);
	}

	if (!strcmp("postbuffer", key)) {
		if (match_is_ignored(matchlen, usermatch, OPT_POST_BUFFER))
			return 0;
		http_post_buffer = git_config_int(var, value);
		if (http_post_buffer < LARGE_PACKET_MAX)
			http_post_buffer = LARGE_PACKET_MAX;
		return 0;
	}

	if (!strcmp("useragent", key)) {
		if (match_is_ignored(matchlen, usermatch, OPT_USER_AGENT))
			return 0;
		return git_config_string(&user_agent, var, value);
	}

	/* Fall back on the default ones */
	return git_default_config(var, value, cb);
}

static void init_curl_http_auth(CURL *result)
{
	if (!http_auth.username)
		return;

	credential_fill(&http_auth);

#if LIBCURL_VERSION_NUM >= 0x071301
	curl_easy_setopt(result, CURLOPT_USERNAME, http_auth.username);
	curl_easy_setopt(result, CURLOPT_PASSWORD, http_auth.password);
#else
	{
		static struct strbuf up = STRBUF_INIT;
		/*
		 * Note that we assume we only ever have a single set of
		 * credentials in a given program run, so we do not have
		 * to worry about updating this buffer, only setting its
		 * initial value.
		 */
		if (!up.len)
			strbuf_addf(&up, "%s:%s",
				http_auth.username, http_auth.password);
		curl_easy_setopt(result, CURLOPT_USERPWD, up.buf);
	}
#endif
}

static int has_cert_password(void)
{
	if (ssl_cert == NULL || ssl_cert_password_required != 1)
		return 0;
	if (!cert_auth.password) {
		cert_auth.protocol = xstrdup("cert");
		cert_auth.username = xstrdup("");
		cert_auth.path = xstrdup(ssl_cert);
		credential_fill(&cert_auth);
	}
	return 1;
}

static CURL *get_curl_handle(void)
{
	CURL *result = curl_easy_init();

	if (!curl_ssl_verify) {
		curl_easy_setopt(result, CURLOPT_SSL_VERIFYPEER, 0);
		curl_easy_setopt(result, CURLOPT_SSL_VERIFYHOST, 0);
	} else {
		/* Verify authenticity of the peer's certificate */
		curl_easy_setopt(result, CURLOPT_SSL_VERIFYPEER, 1);
		/* The name in the cert must match whom we tried to connect */
		curl_easy_setopt(result, CURLOPT_SSL_VERIFYHOST, 2);
	}

#if LIBCURL_VERSION_NUM >= 0x070907
	curl_easy_setopt(result, CURLOPT_NETRC, CURL_NETRC_OPTIONAL);
#endif
#ifdef LIBCURL_CAN_HANDLE_AUTH_ANY
	curl_easy_setopt(result, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
#endif

	if (http_proactive_auth)
		init_curl_http_auth(result);

	if (ssl_cert != NULL)
		curl_easy_setopt(result, CURLOPT_SSLCERT, ssl_cert);
	if (has_cert_password())
		curl_easy_setopt(result, CURLOPT_KEYPASSWD, cert_auth.password);
#if LIBCURL_VERSION_NUM >= 0x070903
	if (ssl_key != NULL)
		curl_easy_setopt(result, CURLOPT_SSLKEY, ssl_key);
#endif
#if LIBCURL_VERSION_NUM >= 0x070908
	if (ssl_capath != NULL)
		curl_easy_setopt(result, CURLOPT_CAPATH, ssl_capath);
#endif
	if (ssl_cainfo != NULL)
		curl_easy_setopt(result, CURLOPT_CAINFO, ssl_cainfo);

	if (curl_low_speed_limit > 0 && curl_low_speed_time > 0) {
		curl_easy_setopt(result, CURLOPT_LOW_SPEED_LIMIT,
				 curl_low_speed_limit);
		curl_easy_setopt(result, CURLOPT_LOW_SPEED_TIME,
				 curl_low_speed_time);
	}

	curl_easy_setopt(result, CURLOPT_FOLLOWLOCATION, 1);
#if LIBCURL_VERSION_NUM >= 0x071301
	curl_easy_setopt(result, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);
#elif LIBCURL_VERSION_NUM >= 0x071101
	curl_easy_setopt(result, CURLOPT_POST301, 1);
#endif

	if (getenv("GIT_CURL_VERBOSE"))
		curl_easy_setopt(result, CURLOPT_VERBOSE, 1);

	curl_easy_setopt(result, CURLOPT_USERAGENT,
		user_agent ? user_agent : git_user_agent());

	if (curl_ftp_no_epsv)
		curl_easy_setopt(result, CURLOPT_FTP_USE_EPSV, 0);

#ifdef CURLOPT_USE_SSL
	if (curl_ssl_try)
		curl_easy_setopt(result, CURLOPT_USE_SSL, CURLUSESSL_TRY);
#endif

	if (curl_http_proxy) {
		curl_easy_setopt(result, CURLOPT_PROXY, curl_http_proxy);
		curl_easy_setopt(result, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
	}

	return result;
}

static void set_from_env(const char **var, const char *envname)
{
	const char *val = getenv(envname);
	if (val)
		*var = val;
}

void http_init(struct remote *remote, const char *url, int proactive_auth)
{
	char *low_speed_limit;
	char *low_speed_time;
	struct url_info info;

	http_is_verbose = 0;

	memset(http_option_max_matched_len, 0, sizeof(http_option_max_matched_len));
	memset(http_option_user_matched, 0, sizeof(http_option_user_matched));
	http_options_url_normalize(url, &info);
	git_config(http_options, &info);
	free(info.url);

	curl_global_init(CURL_GLOBAL_ALL);

	http_proactive_auth = proactive_auth;

	if (remote && remote->http_proxy)
		curl_http_proxy = xstrdup(remote->http_proxy);

	pragma_header = curl_slist_append(pragma_header, "Pragma: no-cache");
	no_pragma_header = curl_slist_append(no_pragma_header, "Pragma:");

#ifdef USE_CURL_MULTI
	{
		char *http_max_requests = getenv("GIT_HTTP_MAX_REQUESTS");
		if (http_max_requests != NULL)
			max_requests = atoi(http_max_requests);
	}

	curlm = curl_multi_init();
	if (curlm == NULL) {
		fprintf(stderr, "Error creating curl multi handle.\n");
		exit(1);
	}
#endif

	if (getenv("GIT_SSL_NO_VERIFY"))
		curl_ssl_verify = 0;

	set_from_env(&ssl_cert, "GIT_SSL_CERT");
#if LIBCURL_VERSION_NUM >= 0x070903
	set_from_env(&ssl_key, "GIT_SSL_KEY");
#endif
#if LIBCURL_VERSION_NUM >= 0x070908
	set_from_env(&ssl_capath, "GIT_SSL_CAPATH");
#endif
	set_from_env(&ssl_cainfo, "GIT_SSL_CAINFO");

	set_from_env(&user_agent, "GIT_HTTP_USER_AGENT");

	low_speed_limit = getenv("GIT_HTTP_LOW_SPEED_LIMIT");
	if (low_speed_limit != NULL)
		curl_low_speed_limit = strtol(low_speed_limit, NULL, 10);
	low_speed_time = getenv("GIT_HTTP_LOW_SPEED_TIME");
	if (low_speed_time != NULL)
		curl_low_speed_time = strtol(low_speed_time, NULL, 10);

	if (curl_ssl_verify == -1)
		curl_ssl_verify = 1;

	curl_session_count = 0;
#ifdef USE_CURL_MULTI
	if (max_requests < 1)
		max_requests = DEFAULT_MAX_REQUESTS;
#endif

	if (getenv("GIT_CURL_FTP_NO_EPSV"))
		curl_ftp_no_epsv = 1;

	if (url) {
		credential_from_url(&http_auth, url);
		if (!ssl_cert_password_required &&
		    getenv("GIT_SSL_CERT_PASSWORD_PROTECTED") &&
		    !prefixcmp(url, "https://"))
			ssl_cert_password_required = 1;
	}

#ifndef NO_CURL_EASY_DUPHANDLE
	curl_default = get_curl_handle();
#endif
}

void http_cleanup(void)
{
	struct active_request_slot *slot = active_queue_head;

	while (slot != NULL) {
		struct active_request_slot *next = slot->next;
		if (slot->curl != NULL) {
#ifdef USE_CURL_MULTI
			curl_multi_remove_handle(curlm, slot->curl);
#endif
			curl_easy_cleanup(slot->curl);
		}
		free(slot);
		slot = next;
	}
	active_queue_head = NULL;

#ifndef NO_CURL_EASY_DUPHANDLE
	curl_easy_cleanup(curl_default);
#endif

#ifdef USE_CURL_MULTI
	curl_multi_cleanup(curlm);
#endif
	curl_global_cleanup();

	curl_slist_free_all(pragma_header);
	pragma_header = NULL;

	curl_slist_free_all(no_pragma_header);
	no_pragma_header = NULL;

	if (curl_http_proxy) {
		free((void *)curl_http_proxy);
		curl_http_proxy = NULL;
	}

	if (cert_auth.password != NULL) {
		memset(cert_auth.password, 0, strlen(cert_auth.password));
		free(cert_auth.password);
		cert_auth.password = NULL;
	}
	ssl_cert_password_required = 0;
}

struct active_request_slot *get_active_slot(void)
{
	struct active_request_slot *slot = active_queue_head;
	struct active_request_slot *newslot;

#ifdef USE_CURL_MULTI
	int num_transfers;

	/* Wait for a slot to open up if the queue is full */
	while (active_requests >= max_requests) {
		curl_multi_perform(curlm, &num_transfers);
		if (num_transfers < active_requests)
			process_curl_messages();
	}
#endif

	while (slot != NULL && slot->in_use)
		slot = slot->next;

	if (slot == NULL) {
		newslot = xmalloc(sizeof(*newslot));
		newslot->curl = NULL;
		newslot->in_use = 0;
		newslot->next = NULL;

		slot = active_queue_head;
		if (slot == NULL) {
			active_queue_head = newslot;
		} else {
			while (slot->next != NULL)
				slot = slot->next;
			slot->next = newslot;
		}
		slot = newslot;
	}

	if (slot->curl == NULL) {
#ifdef NO_CURL_EASY_DUPHANDLE
		slot->curl = get_curl_handle();
#else
		slot->curl = curl_easy_duphandle(curl_default);
#endif
		curl_session_count++;
	}

	active_requests++;
	slot->in_use = 1;
	slot->results = NULL;
	slot->finished = NULL;
	slot->callback_data = NULL;
	slot->callback_func = NULL;
	curl_easy_setopt(slot->curl, CURLOPT_COOKIEFILE, curl_cookie_file);
	curl_easy_setopt(slot->curl, CURLOPT_HTTPHEADER, pragma_header);
	curl_easy_setopt(slot->curl, CURLOPT_ERRORBUFFER, curl_errorstr);
	curl_easy_setopt(slot->curl, CURLOPT_CUSTOMREQUEST, NULL);
	curl_easy_setopt(slot->curl, CURLOPT_READFUNCTION, NULL);
	curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION, NULL);
	curl_easy_setopt(slot->curl, CURLOPT_POSTFIELDS, NULL);
	curl_easy_setopt(slot->curl, CURLOPT_UPLOAD, 0);
	curl_easy_setopt(slot->curl, CURLOPT_HTTPGET, 1);
	curl_easy_setopt(slot->curl, CURLOPT_FAILONERROR, 1);
	if (http_auth.password)
		init_curl_http_auth(slot->curl);

	return slot;
}

int start_active_slot(struct active_request_slot *slot)
{
#ifdef USE_CURL_MULTI
	CURLMcode curlm_result = curl_multi_add_handle(curlm, slot->curl);
	int num_transfers;

	if (curlm_result != CURLM_OK &&
	    curlm_result != CURLM_CALL_MULTI_PERFORM) {
		active_requests--;
		slot->in_use = 0;
		return 0;
	}

	/*
	 * We know there must be something to do, since we just added
	 * something.
	 */
	curl_multi_perform(curlm, &num_transfers);
#endif
	return 1;
}

#ifdef USE_CURL_MULTI
struct fill_chain {
	void *data;
	int (*fill)(void *);
	struct fill_chain *next;
};

static struct fill_chain *fill_cfg;

void add_fill_function(void *data, int (*fill)(void *))
{
	struct fill_chain *new = xmalloc(sizeof(*new));
	struct fill_chain **linkp = &fill_cfg;
	new->data = data;
	new->fill = fill;
	new->next = NULL;
	while (*linkp)
		linkp = &(*linkp)->next;
	*linkp = new;
}

void fill_active_slots(void)
{
	struct active_request_slot *slot = active_queue_head;

	while (active_requests < max_requests) {
		struct fill_chain *fill;
		for (fill = fill_cfg; fill; fill = fill->next)
			if (fill->fill(fill->data))
				break;

		if (!fill)
			break;
	}

	while (slot != NULL) {
		if (!slot->in_use && slot->curl != NULL
			&& curl_session_count > min_curl_sessions) {
			curl_easy_cleanup(slot->curl);
			slot->curl = NULL;
			curl_session_count--;
		}
		slot = slot->next;
	}
}

void step_active_slots(void)
{
	int num_transfers;
	CURLMcode curlm_result;

	do {
		curlm_result = curl_multi_perform(curlm, &num_transfers);
	} while (curlm_result == CURLM_CALL_MULTI_PERFORM);
	if (num_transfers < active_requests) {
		process_curl_messages();
		fill_active_slots();
	}
}
#endif

void run_active_slot(struct active_request_slot *slot)
{
#ifdef USE_CURL_MULTI
	fd_set readfds;
	fd_set writefds;
	fd_set excfds;
	int max_fd;
	struct timeval select_timeout;
	int finished = 0;

	slot->finished = &finished;
	while (!finished) {
		step_active_slots();

		if (slot->in_use) {
#if LIBCURL_VERSION_NUM >= 0x070f04
			long curl_timeout;
			curl_multi_timeout(curlm, &curl_timeout);
			if (curl_timeout == 0) {
				continue;
			} else if (curl_timeout == -1) {
				select_timeout.tv_sec  = 0;
				select_timeout.tv_usec = 50000;
			} else {
				select_timeout.tv_sec  =  curl_timeout / 1000;
				select_timeout.tv_usec = (curl_timeout % 1000) * 1000;
			}
#else
			select_timeout.tv_sec  = 0;
			select_timeout.tv_usec = 50000;
#endif

			max_fd = -1;
			FD_ZERO(&readfds);
			FD_ZERO(&writefds);
			FD_ZERO(&excfds);
			curl_multi_fdset(curlm, &readfds, &writefds, &excfds, &max_fd);

			/*
			 * It can happen that curl_multi_timeout returns a pathologically
			 * long timeout when curl_multi_fdset returns no file descriptors
			 * to read.  See commit message for more details.
			 */
			if (max_fd < 0 &&
			    (select_timeout.tv_sec > 0 ||
			     select_timeout.tv_usec > 50000)) {
				select_timeout.tv_sec  = 0;
				select_timeout.tv_usec = 50000;
			}

			select(max_fd+1, &readfds, &writefds, &excfds, &select_timeout);
		}
	}
#else
	while (slot->in_use) {
		slot->curl_result = curl_easy_perform(slot->curl);
		finish_active_slot(slot);
	}
#endif
}

static void closedown_active_slot(struct active_request_slot *slot)
{
	active_requests--;
	slot->in_use = 0;
}

static void release_active_slot(struct active_request_slot *slot)
{
	closedown_active_slot(slot);
	if (slot->curl && curl_session_count > min_curl_sessions) {
#ifdef USE_CURL_MULTI
		curl_multi_remove_handle(curlm, slot->curl);
#endif
		curl_easy_cleanup(slot->curl);
		slot->curl = NULL;
		curl_session_count--;
	}
#ifdef USE_CURL_MULTI
	fill_active_slots();
#endif
}

void finish_active_slot(struct active_request_slot *slot)
{
	closedown_active_slot(slot);
	curl_easy_getinfo(slot->curl, CURLINFO_HTTP_CODE, &slot->http_code);

	if (slot->finished != NULL)
		(*slot->finished) = 1;

	/* Store slot results so they can be read after the slot is reused */
	if (slot->results != NULL) {
		slot->results->curl_result = slot->curl_result;
		slot->results->http_code = slot->http_code;
	}

	/* Run callback if appropriate */
	if (slot->callback_func != NULL)
		slot->callback_func(slot->callback_data);
}

void finish_all_active_slots(void)
{
	struct active_request_slot *slot = active_queue_head;

	while (slot != NULL)
		if (slot->in_use) {
			run_active_slot(slot);
			slot = active_queue_head;
		} else {
			slot = slot->next;
		}
}

/* Helpers for modifying and creating URLs */
static inline int needs_quote(int ch)
{
	if (((ch >= 'A') && (ch <= 'Z'))
			|| ((ch >= 'a') && (ch <= 'z'))
			|| ((ch >= '0') && (ch <= '9'))
			|| (ch == '/')
			|| (ch == '-')
			|| (ch == '.'))
		return 0;
	return 1;
}

static char *quote_ref_url(const char *base, const char *ref)
{
	struct strbuf buf = STRBUF_INIT;
	const char *cp;
	int ch;

	end_url_with_slash(&buf, base);

	for (cp = ref; (ch = *cp) != 0; cp++)
		if (needs_quote(ch))
			strbuf_addf(&buf, "%%%02x", ch);
		else
			strbuf_addch(&buf, *cp);

	return strbuf_detach(&buf, NULL);
}

void append_remote_object_url(struct strbuf *buf, const char *url,
			      const char *hex,
			      int only_two_digit_prefix)
{
	end_url_with_slash(buf, url);

	strbuf_addf(buf, "objects/%.*s/", 2, hex);
	if (!only_two_digit_prefix)
		strbuf_addf(buf, "%s", hex+2);
}

char *get_remote_object_url(const char *url, const char *hex,
			    int only_two_digit_prefix)
{
	struct strbuf buf = STRBUF_INIT;
	append_remote_object_url(&buf, url, hex, only_two_digit_prefix);
	return strbuf_detach(&buf, NULL);
}

int handle_curl_result(struct slot_results *results)
{
	/*
	 * If we see a failing http code with CURLE_OK, we have turned off
	 * FAILONERROR (to keep the server's custom error response), and should
	 * translate the code into failure here.
	 */
	if (results->curl_result == CURLE_OK &&
	    results->http_code >= 400) {
		results->curl_result = CURLE_HTTP_RETURNED_ERROR;
		/*
		 * Normally curl will already have put the "reason phrase"
		 * from the server into curl_errorstr; unfortunately without
		 * FAILONERROR it is lost, so we can give only the numeric
		 * status code.
		 */
		snprintf(curl_errorstr, sizeof(curl_errorstr),
			 "The requested URL returned error: %ld",
			 results->http_code);
	}

	if (results->curl_result == CURLE_OK) {
		credential_approve(&http_auth);
		return HTTP_OK;
	} else if (missing_target(results))
		return HTTP_MISSING_TARGET;
	else if (results->http_code == 401) {
		if (http_auth.username && http_auth.password) {
			credential_reject(&http_auth);
			return HTTP_NOAUTH;
		} else {
			credential_fill(&http_auth);
			return HTTP_REAUTH;
		}
	} else {
#if LIBCURL_VERSION_NUM >= 0x070c00
		if (!curl_errorstr[0])
			strlcpy(curl_errorstr,
				curl_easy_strerror(results->curl_result),
				sizeof(curl_errorstr));
#endif
		return HTTP_ERROR;
	}
}

/* http_request() targets */
#define HTTP_REQUEST_STRBUF	0
#define HTTP_REQUEST_FILE	1

static int http_request(const char *url, struct strbuf *type,
			void *result, int target, int options)
{
	struct active_request_slot *slot;
	struct slot_results results;
	struct curl_slist *headers = NULL;
	struct strbuf buf = STRBUF_INIT;
	int ret;

	slot = get_active_slot();
	slot->results = &results;
	curl_easy_setopt(slot->curl, CURLOPT_HTTPGET, 1);

	if (result == NULL) {
		curl_easy_setopt(slot->curl, CURLOPT_NOBODY, 1);
	} else {
		curl_easy_setopt(slot->curl, CURLOPT_NOBODY, 0);
		curl_easy_setopt(slot->curl, CURLOPT_FILE, result);

		if (target == HTTP_REQUEST_FILE) {
			long posn = ftell(result);
			curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION,
					 fwrite);
			if (posn > 0) {
				strbuf_addf(&buf, "Range: bytes=%ld-", posn);
				headers = curl_slist_append(headers, buf.buf);
				strbuf_reset(&buf);
			}
		} else
			curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION,
					 fwrite_buffer);
	}

	strbuf_addstr(&buf, "Pragma:");
	if (options & HTTP_NO_CACHE)
		strbuf_addstr(&buf, " no-cache");
	if (options & HTTP_KEEP_ERROR)
		curl_easy_setopt(slot->curl, CURLOPT_FAILONERROR, 0);

	headers = curl_slist_append(headers, buf.buf);

	curl_easy_setopt(slot->curl, CURLOPT_URL, url);
	curl_easy_setopt(slot->curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(slot->curl, CURLOPT_ENCODING, "gzip");

	if (start_active_slot(slot)) {
		run_active_slot(slot);
		ret = handle_curl_result(&results);
	} else {
		snprintf(curl_errorstr, sizeof(curl_errorstr),
			 "failed to start HTTP request");
		ret = HTTP_START_FAILED;
	}

	if (type) {
		char *t;
		strbuf_reset(type);
		curl_easy_getinfo(slot->curl, CURLINFO_CONTENT_TYPE, &t);
		if (t)
			strbuf_addstr(type, t);
	}

	curl_slist_free_all(headers);
	strbuf_release(&buf);

	return ret;
}

static int http_request_reauth(const char *url,
			       struct strbuf *type,
			       void *result, int target,
			       int options)
{
	int ret = http_request(url, type, result, target, options);
	if (ret != HTTP_REAUTH)
		return ret;

	/*
	 * If we are using KEEP_ERROR, the previous request may have
	 * put cruft into our output stream; we should clear it out before
	 * making our next request. We only know how to do this for
	 * the strbuf case, but that is enough to satisfy current callers.
	 */
	if (options & HTTP_KEEP_ERROR) {
		switch (target) {
		case HTTP_REQUEST_STRBUF:
			strbuf_reset(result);
			break;
		default:
			die("BUG: HTTP_KEEP_ERROR is only supported with strbufs");
		}
	}
	return http_request(url, type, result, target, options);
}

int http_get_strbuf(const char *url,
		    struct strbuf *type,
		    struct strbuf *result, int options)
{
	return http_request_reauth(url, type, result,
				   HTTP_REQUEST_STRBUF, options);
}

/*
 * Downloads a URL and stores the result in the given file.
 *
 * If a previous interrupted download is detected (i.e. a previous temporary
 * file is still around) the download is resumed.
 */
static int http_get_file(const char *url, const char *filename, int options)
{
	int ret;
	struct strbuf tmpfile = STRBUF_INIT;
	FILE *result;

	strbuf_addf(&tmpfile, "%s.temp", filename);
	result = fopen(tmpfile.buf, "a");
	if (! result) {
		error("Unable to open local file %s", tmpfile.buf);
		ret = HTTP_ERROR;
		goto cleanup;
	}

	ret = http_request_reauth(url, NULL, result, HTTP_REQUEST_FILE, options);
	fclose(result);

	if ((ret == HTTP_OK) && move_temp_to_file(tmpfile.buf, filename))
		ret = HTTP_ERROR;
cleanup:
	strbuf_release(&tmpfile);
	return ret;
}

int http_fetch_ref(const char *base, struct ref *ref)
{
	char *url;
	struct strbuf buffer = STRBUF_INIT;
	int ret = -1;

	url = quote_ref_url(base, ref->name);
	if (http_get_strbuf(url, NULL, &buffer, HTTP_NO_CACHE) == HTTP_OK) {
		strbuf_rtrim(&buffer);
		if (buffer.len == 40)
			ret = get_sha1_hex(buffer.buf, ref->old_sha1);
		else if (!prefixcmp(buffer.buf, "ref: ")) {
			ref->symref = xstrdup(buffer.buf + 5);
			ret = 0;
		}
	}

	strbuf_release(&buffer);
	free(url);
	return ret;
}

/* Helpers for fetching packs */
static char *fetch_pack_index(unsigned char *sha1, const char *base_url)
{
	char *url, *tmp;
	struct strbuf buf = STRBUF_INIT;

	if (http_is_verbose)
		fprintf(stderr, "Getting index for pack %s\n", sha1_to_hex(sha1));

	end_url_with_slash(&buf, base_url);
	strbuf_addf(&buf, "objects/pack/pack-%s.idx", sha1_to_hex(sha1));
	url = strbuf_detach(&buf, NULL);

	strbuf_addf(&buf, "%s.temp", sha1_pack_index_name(sha1));
	tmp = strbuf_detach(&buf, NULL);

	if (http_get_file(url, tmp, 0) != HTTP_OK) {
		error("Unable to get pack index %s", url);
		free(tmp);
		tmp = NULL;
	}

	free(url);
	return tmp;
}

static int fetch_and_setup_pack_index(struct packed_git **packs_head,
	unsigned char *sha1, const char *base_url)
{
	struct packed_git *new_pack;
	char *tmp_idx = NULL;
	int ret;

	if (has_pack_index(sha1)) {
		new_pack = parse_pack_index(sha1, NULL);
		if (!new_pack)
			return -1; /* parse_pack_index() already issued error message */
		goto add_pack;
	}

	tmp_idx = fetch_pack_index(sha1, base_url);
	if (!tmp_idx)
		return -1;

	new_pack = parse_pack_index(sha1, tmp_idx);
	if (!new_pack) {
		unlink(tmp_idx);
		free(tmp_idx);

		return -1; /* parse_pack_index() already issued error message */
	}

	ret = verify_pack_index(new_pack);
	if (!ret) {
		close_pack_index(new_pack);
		ret = move_temp_to_file(tmp_idx, sha1_pack_index_name(sha1));
	}
	free(tmp_idx);
	if (ret)
		return -1;

add_pack:
	new_pack->next = *packs_head;
	*packs_head = new_pack;
	return 0;
}

int http_get_info_packs(const char *base_url, struct packed_git **packs_head)
{
	int ret = 0, i = 0;
	char *url, *data;
	struct strbuf buf = STRBUF_INIT;
	unsigned char sha1[20];

	end_url_with_slash(&buf, base_url);
	strbuf_addstr(&buf, "objects/info/packs");
	url = strbuf_detach(&buf, NULL);

	ret = http_get_strbuf(url, NULL, &buf, HTTP_NO_CACHE);
	if (ret != HTTP_OK)
		goto cleanup;

	data = buf.buf;
	while (i < buf.len) {
		switch (data[i]) {
		case 'P':
			i++;
			if (i + 52 <= buf.len &&
			    !prefixcmp(data + i, " pack-") &&
			    !prefixcmp(data + i + 46, ".pack\n")) {
				get_sha1_hex(data + i + 6, sha1);
				fetch_and_setup_pack_index(packs_head, sha1,
						      base_url);
				i += 51;
				break;
			}
		default:
			while (i < buf.len && data[i] != '\n')
				i++;
		}
		i++;
	}

cleanup:
	free(url);
	return ret;
}

void release_http_pack_request(struct http_pack_request *preq)
{
	if (preq->packfile != NULL) {
		fclose(preq->packfile);
		preq->packfile = NULL;
	}
	if (preq->range_header != NULL) {
		curl_slist_free_all(preq->range_header);
		preq->range_header = NULL;
	}
	preq->slot = NULL;
	free(preq->url);
}

int finish_http_pack_request(struct http_pack_request *preq)
{
	struct packed_git **lst;
	struct packed_git *p = preq->target;
	char *tmp_idx;
	struct child_process ip;
	const char *ip_argv[8];

	close_pack_index(p);

	fclose(preq->packfile);
	preq->packfile = NULL;

	lst = preq->lst;
	while (*lst != p)
		lst = &((*lst)->next);
	*lst = (*lst)->next;

	tmp_idx = xstrdup(preq->tmpfile);
	strcpy(tmp_idx + strlen(tmp_idx) - strlen(".pack.temp"),
	       ".idx.temp");

	ip_argv[0] = "index-pack";
	ip_argv[1] = "-o";
	ip_argv[2] = tmp_idx;
	ip_argv[3] = preq->tmpfile;
	ip_argv[4] = NULL;

	memset(&ip, 0, sizeof(ip));
	ip.argv = ip_argv;
	ip.git_cmd = 1;
	ip.no_stdin = 1;
	ip.no_stdout = 1;

	if (run_command(&ip)) {
		unlink(preq->tmpfile);
		unlink(tmp_idx);
		free(tmp_idx);
		return -1;
	}

	unlink(sha1_pack_index_name(p->sha1));

	if (move_temp_to_file(preq->tmpfile, sha1_pack_name(p->sha1))
	 || move_temp_to_file(tmp_idx, sha1_pack_index_name(p->sha1))) {
		free(tmp_idx);
		return -1;
	}

	install_packed_git(p);
	free(tmp_idx);
	return 0;
}

struct http_pack_request *new_http_pack_request(
	struct packed_git *target, const char *base_url)
{
	long prev_posn = 0;
	char range[RANGE_HEADER_SIZE];
	struct strbuf buf = STRBUF_INIT;
	struct http_pack_request *preq;

	preq = xcalloc(1, sizeof(*preq));
	preq->target = target;

	end_url_with_slash(&buf, base_url);
	strbuf_addf(&buf, "objects/pack/pack-%s.pack",
		sha1_to_hex(target->sha1));
	preq->url = strbuf_detach(&buf, NULL);

	snprintf(preq->tmpfile, sizeof(preq->tmpfile), "%s.temp",
		sha1_pack_name(target->sha1));
	preq->packfile = fopen(preq->tmpfile, "a");
	if (!preq->packfile) {
		error("Unable to open local file %s for pack",
		      preq->tmpfile);
		goto abort;
	}

	preq->slot = get_active_slot();
	curl_easy_setopt(preq->slot->curl, CURLOPT_FILE, preq->packfile);
	curl_easy_setopt(preq->slot->curl, CURLOPT_WRITEFUNCTION, fwrite);
	curl_easy_setopt(preq->slot->curl, CURLOPT_URL, preq->url);
	curl_easy_setopt(preq->slot->curl, CURLOPT_HTTPHEADER,
		no_pragma_header);

	/*
	 * If there is data present from a previous transfer attempt,
	 * resume where it left off
	 */
	prev_posn = ftell(preq->packfile);
	if (prev_posn>0) {
		if (http_is_verbose)
			fprintf(stderr,
				"Resuming fetch of pack %s at byte %ld\n",
				sha1_to_hex(target->sha1), prev_posn);
		sprintf(range, "Range: bytes=%ld-", prev_posn);
		preq->range_header = curl_slist_append(NULL, range);
		curl_easy_setopt(preq->slot->curl, CURLOPT_HTTPHEADER,
			preq->range_header);
	}

	return preq;

abort:
	free(preq->url);
	free(preq);
	return NULL;
}

/* Helpers for fetching objects (loose) */
static size_t fwrite_sha1_file(char *ptr, size_t eltsize, size_t nmemb,
			       void *data)
{
	unsigned char expn[4096];
	size_t size = eltsize * nmemb;
	int posn = 0;
	struct http_object_request *freq =
		(struct http_object_request *)data;
	do {
		ssize_t retval = xwrite(freq->localfile,
					(char *) ptr + posn, size - posn);
		if (retval < 0)
			return posn;
		posn += retval;
	} while (posn < size);

	freq->stream.avail_in = size;
	freq->stream.next_in = (void *)ptr;
	do {
		freq->stream.next_out = expn;
		freq->stream.avail_out = sizeof(expn);
		freq->zret = git_inflate(&freq->stream, Z_SYNC_FLUSH);
		git_SHA1_Update(&freq->c, expn,
				sizeof(expn) - freq->stream.avail_out);
	} while (freq->stream.avail_in && freq->zret == Z_OK);
	return size;
}

struct http_object_request *new_http_object_request(const char *base_url,
	unsigned char *sha1)
{
	char *hex = sha1_to_hex(sha1);
	char *filename;
	char prevfile[PATH_MAX];
	int prevlocal;
	char prev_buf[PREV_BUF_SIZE];
	ssize_t prev_read = 0;
	long prev_posn = 0;
	char range[RANGE_HEADER_SIZE];
	struct curl_slist *range_header = NULL;
	struct http_object_request *freq;

	freq = xcalloc(1, sizeof(*freq));
	hashcpy(freq->sha1, sha1);
	freq->localfile = -1;

	filename = sha1_file_name(sha1);
	snprintf(freq->tmpfile, sizeof(freq->tmpfile),
		 "%s.temp", filename);

	snprintf(prevfile, sizeof(prevfile), "%s.prev", filename);
	unlink_or_warn(prevfile);
	rename(freq->tmpfile, prevfile);
	unlink_or_warn(freq->tmpfile);

	if (freq->localfile != -1)
		error("fd leakage in start: %d", freq->localfile);
	freq->localfile = open(freq->tmpfile,
			       O_WRONLY | O_CREAT | O_EXCL, 0666);
	/*
	 * This could have failed due to the "lazy directory creation";
	 * try to mkdir the last path component.
	 */
	if (freq->localfile < 0 && errno == ENOENT) {
		char *dir = strrchr(freq->tmpfile, '/');
		if (dir) {
			*dir = 0;
			mkdir(freq->tmpfile, 0777);
			*dir = '/';
		}
		freq->localfile = open(freq->tmpfile,
				       O_WRONLY | O_CREAT | O_EXCL, 0666);
	}

	if (freq->localfile < 0) {
		error("Couldn't create temporary file %s: %s",
		      freq->tmpfile, strerror(errno));
		goto abort;
	}

	git_inflate_init(&freq->stream);

	git_SHA1_Init(&freq->c);

	freq->url = get_remote_object_url(base_url, hex, 0);

	/*
	 * If a previous temp file is present, process what was already
	 * fetched.
	 */
	prevlocal = open(prevfile, O_RDONLY);
	if (prevlocal != -1) {
		do {
			prev_read = xread(prevlocal, prev_buf, PREV_BUF_SIZE);
			if (prev_read>0) {
				if (fwrite_sha1_file(prev_buf,
						     1,
						     prev_read,
						     freq) == prev_read) {
					prev_posn += prev_read;
				} else {
					prev_read = -1;
				}
			}
		} while (prev_read > 0);
		close(prevlocal);
	}
	unlink_or_warn(prevfile);

	/*
	 * Reset inflate/SHA1 if there was an error reading the previous temp
	 * file; also rewind to the beginning of the local file.
	 */
	if (prev_read == -1) {
		memset(&freq->stream, 0, sizeof(freq->stream));
		git_inflate_init(&freq->stream);
		git_SHA1_Init(&freq->c);
		if (prev_posn>0) {
			prev_posn = 0;
			lseek(freq->localfile, 0, SEEK_SET);
			if (ftruncate(freq->localfile, 0) < 0) {
				error("Couldn't truncate temporary file %s: %s",
					  freq->tmpfile, strerror(errno));
				goto abort;
			}
		}
	}

	freq->slot = get_active_slot();

	curl_easy_setopt(freq->slot->curl, CURLOPT_FILE, freq);
	curl_easy_setopt(freq->slot->curl, CURLOPT_WRITEFUNCTION, fwrite_sha1_file);
	curl_easy_setopt(freq->slot->curl, CURLOPT_ERRORBUFFER, freq->errorstr);
	curl_easy_setopt(freq->slot->curl, CURLOPT_URL, freq->url);
	curl_easy_setopt(freq->slot->curl, CURLOPT_HTTPHEADER, no_pragma_header);

	/*
	 * If we have successfully processed data from a previous fetch
	 * attempt, only fetch the data we don't already have.
	 */
	if (prev_posn>0) {
		if (http_is_verbose)
			fprintf(stderr,
				"Resuming fetch of object %s at byte %ld\n",
				hex, prev_posn);
		sprintf(range, "Range: bytes=%ld-", prev_posn);
		range_header = curl_slist_append(range_header, range);
		curl_easy_setopt(freq->slot->curl,
				 CURLOPT_HTTPHEADER, range_header);
	}

	return freq;

abort:
	free(freq->url);
	free(freq);
	return NULL;
}

void process_http_object_request(struct http_object_request *freq)
{
	if (freq->slot == NULL)
		return;
	freq->curl_result = freq->slot->curl_result;
	freq->http_code = freq->slot->http_code;
	freq->slot = NULL;
}

int finish_http_object_request(struct http_object_request *freq)
{
	struct stat st;

	close(freq->localfile);
	freq->localfile = -1;

	process_http_object_request(freq);

	if (freq->http_code == 416) {
		warning("requested range invalid; we may already have all the data.");
	} else if (freq->curl_result != CURLE_OK) {
		if (stat(freq->tmpfile, &st) == 0)
			if (st.st_size == 0)
				unlink_or_warn(freq->tmpfile);
		return -1;
	}

	git_inflate_end(&freq->stream);
	git_SHA1_Final(freq->real_sha1, &freq->c);
	if (freq->zret != Z_STREAM_END) {
		unlink_or_warn(freq->tmpfile);
		return -1;
	}
	if (hashcmp(freq->sha1, freq->real_sha1)) {
		unlink_or_warn(freq->tmpfile);
		return -1;
	}
	freq->rename =
		move_temp_to_file(freq->tmpfile, sha1_file_name(freq->sha1));

	return freq->rename;
}

void abort_http_object_request(struct http_object_request *freq)
{
	unlink_or_warn(freq->tmpfile);

	release_http_object_request(freq);
}

void release_http_object_request(struct http_object_request *freq)
{
	if (freq->localfile != -1) {
		close(freq->localfile);
		freq->localfile = -1;
	}
	if (freq->url != NULL) {
		free(freq->url);
		freq->url = NULL;
	}
	if (freq->slot != NULL) {
		freq->slot->callback_func = NULL;
		freq->slot->callback_data = NULL;
		release_active_slot(freq->slot);
		freq->slot = NULL;
	}
}
