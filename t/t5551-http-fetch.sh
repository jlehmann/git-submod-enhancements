#!/bin/sh

test_description='test smart fetching over http via http-backend'
. ./test-lib.sh

if test -n "$NO_CURL"; then
	skip_all='skipping test, git built without http support'
	test_done
fi

LIB_HTTPD_PORT=${LIB_HTTPD_PORT-'5551'}
. "$TEST_DIRECTORY"/lib-httpd.sh
start_httpd

test_expect_success 'setup repository' '
	git config push.default matching &&
	echo content >file &&
	git add file &&
	git commit -m one
'

test_expect_success 'create http-accessible bare repository' '
	mkdir "$HTTPD_DOCUMENT_ROOT_PATH/repo.git" &&
	(cd "$HTTPD_DOCUMENT_ROOT_PATH/repo.git" &&
	 git --bare init
	) &&
	git remote add public "$HTTPD_DOCUMENT_ROOT_PATH/repo.git" &&
	git push public master:master
'

setup_askpass_helper

cat >exp <<EOF
> GET /smart/repo.git/info/refs?service=git-upload-pack HTTP/1.1
> Accept: */*
> Accept-Encoding: gzip
> Pragma: no-cache
< HTTP/1.1 200 OK
< Pragma: no-cache
< Cache-Control: no-cache, max-age=0, must-revalidate
< Content-Type: application/x-git-upload-pack-advertisement
> POST /smart/repo.git/git-upload-pack HTTP/1.1
> Accept-Encoding: gzip
> Content-Type: application/x-git-upload-pack-request
> Accept: application/x-git-upload-pack-result
> Content-Length: xxx
< HTTP/1.1 200 OK
< Pragma: no-cache
< Cache-Control: no-cache, max-age=0, must-revalidate
< Content-Type: application/x-git-upload-pack-result
EOF
test_expect_success 'clone http repository' '
	GIT_CURL_VERBOSE=1 git clone --quiet $HTTPD_URL/smart/repo.git clone 2>err &&
	test_cmp file clone/file &&
	tr '\''\015'\'' Q <err |
	sed -e "
		s/Q\$//
		/^[*] /d
		/^$/d
		/^< $/d

		/^[^><]/{
			s/^/> /
		}

		/^> User-Agent: /d
		/^> Host: /d
		/^> POST /,$ {
			/^> Accept: [*]\\/[*]/d
		}
		s/^> Content-Length: .*/> Content-Length: xxx/
		/^> 00..want /d
		/^> 00.*done/d

		/^< Server: /d
		/^< Expires: /d
		/^< Date: /d
		/^< Content-Length: /d
		/^< Transfer-Encoding: /d
	" >act &&
	test_cmp exp act
'

test_expect_success 'fetch changes via http' '
	echo content >>file &&
	git commit -a -m two &&
	git push public
	(cd clone && git pull) &&
	test_cmp file clone/file
'

cat >exp <<EOF
GET  /smart/repo.git/info/refs?service=git-upload-pack HTTP/1.1 200
POST /smart/repo.git/git-upload-pack HTTP/1.1 200
GET  /smart/repo.git/info/refs?service=git-upload-pack HTTP/1.1 200
POST /smart/repo.git/git-upload-pack HTTP/1.1 200
EOF
test_expect_success 'used upload-pack service' '
	sed -e "
		s/^.* \"//
		s/\"//
		s/ [1-9][0-9]*\$//
		s/^GET /GET  /
	" >act <"$HTTPD_ROOT_PATH"/access.log &&
	test_cmp exp act
'

test_expect_success 'follow redirects (301)' '
	git clone $HTTPD_URL/smart-redir-perm/repo.git --quiet repo-p
'

test_expect_success 'follow redirects (302)' '
	git clone $HTTPD_URL/smart-redir-temp/repo.git --quiet repo-t
'

test_expect_success 'clone from password-protected repository' '
	echo two >expect &&
	set_askpass user@host &&
	git clone --bare "$HTTPD_URL/auth/smart/repo.git" smart-auth &&
	expect_askpass both user@host &&
	git --git-dir=smart-auth log -1 --format=%s >actual &&
	test_cmp expect actual
'

test_expect_success 'clone from auth-only-for-push repository' '
	echo two >expect &&
	set_askpass wrong &&
	git clone --bare "$HTTPD_URL/auth-push/smart/repo.git" smart-noauth &&
	expect_askpass none &&
	git --git-dir=smart-noauth log -1 --format=%s >actual &&
	test_cmp expect actual
'

test_expect_success 'clone from auth-only-for-objects repository' '
	echo two >expect &&
	set_askpass user@host &&
	git clone --bare "$HTTPD_URL/auth-fetch/smart/repo.git" half-auth &&
	expect_askpass both user@host &&
	git --git-dir=half-auth log -1 --format=%s >actual &&
	test_cmp expect actual
'

test_expect_success 'no-op half-auth fetch does not require a password' '
	set_askpass wrong &&
	git --git-dir=half-auth fetch &&
	expect_askpass none
'

test_expect_success 'disable dumb http on server' '
	git --git-dir="$HTTPD_DOCUMENT_ROOT_PATH/repo.git" \
		config http.getanyfile false
'

test_expect_success 'GIT_SMART_HTTP can disable smart http' '
	(GIT_SMART_HTTP=0 &&
	 export GIT_SMART_HTTP &&
	 cd clone &&
	 test_must_fail git fetch)
'

test_expect_success 'invalid Content-Type rejected' '
	test_must_fail git clone $HTTPD_URL/broken_smart/repo.git 2>actual
	grep "not valid:" actual
'

test_expect_success 'create namespaced refs' '
	test_commit namespaced &&
	git push public HEAD:refs/namespaces/ns/refs/heads/master &&
	git --git-dir="$HTTPD_DOCUMENT_ROOT_PATH/repo.git" \
		symbolic-ref refs/namespaces/ns/HEAD refs/namespaces/ns/refs/heads/master
'

test_expect_success 'smart clone respects namespace' '
	git clone "$HTTPD_URL/smart_namespace/repo.git" ns-smart &&
	echo namespaced >expect &&
	git --git-dir=ns-smart/.git log -1 --format=%s >actual &&
	test_cmp expect actual
'

test_expect_success 'dumb clone via http-backend respects namespace' '
	git --git-dir="$HTTPD_DOCUMENT_ROOT_PATH/repo.git" \
		config http.getanyfile true &&
	GIT_SMART_HTTP=0 git clone \
		"$HTTPD_URL/smart_namespace/repo.git" ns-dumb &&
	echo namespaced >expect &&
	git --git-dir=ns-dumb/.git log -1 --format=%s >actual &&
	test_cmp expect actual
'

cat >cookies.txt <<EOF
127.0.0.1	FALSE	/smart_cookies/	FALSE	0	othername	othervalue
EOF
cat >expect_cookies.txt <<EOF
# Netscape HTTP Cookie File
# http://curl.haxx.se/docs/http-cookies.html
# This file was generated by libcurl! Edit at your own risk.

127.0.0.1	FALSE	/smart_cookies/	FALSE	0	othername	othervalue
127.0.0.1	FALSE	/smart_cookies/repo.git/info/	FALSE	0	name	value
EOF
test_expect_success 'cookies stored in http.cookiefile when http.savecookies set' '
	git config http.cookiefile cookies.txt &&
	git config http.savecookies true &&
	git ls-remote $HTTPD_URL/smart_cookies/repo.git master &&
	test_cmp expect_cookies.txt cookies.txt
'

test -n "$GIT_TEST_LONG" && test_set_prereq EXPENSIVE

test_expect_success EXPENSIVE 'create 50,000 tags in the repo' '
	(
	cd "$HTTPD_DOCUMENT_ROOT_PATH/repo.git" &&
	for i in `test_seq 50000`
	do
		echo "commit refs/heads/too-many-refs"
		echo "mark :$i"
		echo "committer git <git@example.com> $i +0000"
		echo "data 0"
		echo "M 644 inline bla.txt"
		echo "data 4"
		echo "bla"
		# make every commit dangling by always
		# rewinding the branch after each commit
		echo "reset refs/heads/too-many-refs"
		echo "from :1"
	done | git fast-import --export-marks=marks &&

	# now assign tags to all the dangling commits we created above
	tag=$("$PERL_PATH" -e "print \"bla\" x 30") &&
	sed -e "s|^:\([^ ]*\) \(.*\)$|\2 refs/tags/$tag-\1|" <marks >>packed-refs
	)
'

test_expect_success EXPENSIVE 'clone the 50,000 tag repo to check OS command line overflow' '
	git clone $HTTPD_URL/smart/repo.git too-many-refs 2>err &&
	test_line_count = 0 err &&
	(
		cd too-many-refs &&
		test $(git for-each-ref refs/tags | wc -l) = 50000
	)
'

stop_httpd
test_done
