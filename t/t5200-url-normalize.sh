#!/bin/sh

test_description='url normalization'
. ./test-lib.sh

if test -n "$NO_CURL"; then
	skip_all='skipping test, git built without http support'
	test_done
fi

# The base name of the test url files
tu="$TEST_DIRECTORY/t5200/url"

# The base name of the test config files
tc="$TEST_DIRECTORY/t5200/config"

# Note that only file: URLs should be allowed without a host

test_expect_success 'url scheme' '
	! test-url-normalize "" &&
	! test-url-normalize "_" &&
	! test-url-normalize "scheme" &&
	! test-url-normalize "scheme:" &&
	! test-url-normalize "scheme:/" &&
	! test-url-normalize "scheme://" &&
	! test-url-normalize "file" &&
	! test-url-normalize "file:" &&
	! test-url-normalize "file:/" &&
	test-url-normalize "file://" &&
	! test-url-normalize "://acme.co" &&
	! test-url-normalize "x_test://acme.co" &&
	! test-url-normalize "-test://acme.co" &&
	! test-url-normalize "0test://acme.co" &&
	! test-url-normalize "+test://acme.co" &&
	! test-url-normalize ".test://acme.co" &&
	! test-url-normalize "schem%6e://" &&
	test-url-normalize "x-Test+v1.0://acme.co" &&
	test "$(test-url-normalize -p "AbCdeF://x.Y")" = "abcdef://x.y/"
'

test_expect_success 'url authority' '
	! test-url-normalize "scheme://user:pass@" &&
	! test-url-normalize "scheme://?" &&
	! test-url-normalize "scheme://#" &&
	! test-url-normalize "scheme:///" &&
	! test-url-normalize "scheme://:" &&
	! test-url-normalize "scheme://:555" &&
	test-url-normalize "file://user:pass@" &&
	test-url-normalize "file://?" &&
	test-url-normalize "file://#" &&
	test-url-normalize "file:///" &&
	test-url-normalize "file://:" &&
	! test-url-normalize "file://:555" &&
	test-url-normalize "scheme://user:pass@host" &&
	test-url-normalize "scheme://@host" &&
	test-url-normalize "scheme://%00@host" &&
	! test-url-normalize "scheme://%%@host" &&
	! test-url-normalize "scheme://host_" &&
	test-url-normalize "scheme://user:pass@host/" &&
	test-url-normalize "scheme://@host/" &&
	test-url-normalize "scheme://host/" &&
	test-url-normalize "scheme://host?x" &&
	test-url-normalize "scheme://host#x" &&
	test-url-normalize "scheme://host/@" &&
	test-url-normalize "scheme://host?@x" &&
	test-url-normalize "scheme://host#@x" &&
	test-url-normalize "scheme://[::1]" &&
	test-url-normalize "scheme://[::1]/" &&
	! test-url-normalize "scheme://hos%41/" &&
	test-url-normalize "scheme://[invalid....:/" &&
	test-url-normalize "scheme://invalid....:]/" &&
	! test-url-normalize "scheme://invalid....:[/" &&
	! test-url-normalize "scheme://invalid....:["
'

test_expect_success 'url port checks' '
	test-url-normalize "xyz://q@some.host:" &&
	test-url-normalize "xyz://q@some.host:456/" &&
	! test-url-normalize "xyz://q@some.host:0" &&
	! test-url-normalize "xyz://q@some.host:0000000" &&
	test-url-normalize "xyz://q@some.host:0000001?" &&
	test-url-normalize "xyz://q@some.host:065535#" &&
	test-url-normalize "xyz://q@some.host:65535" &&
	! test-url-normalize "xyz://q@some.host:65536" &&
	! test-url-normalize "xyz://q@some.host:99999" &&
	! test-url-normalize "xyz://q@some.host:100000" &&
	! test-url-normalize "xyz://q@some.host:100001" &&
	test-url-normalize "http://q@some.host:80" &&
	test-url-normalize "https://q@some.host:443" &&
	test-url-normalize "http://q@some.host:80/" &&
	test-url-normalize "https://q@some.host:443?" &&
	! test-url-normalize "http://q@:8008" &&
	! test-url-normalize "http://:8080" &&
	! test-url-normalize "http://:" &&
	test-url-normalize "xyz://q@some.host:456/" &&
	test-url-normalize "xyz://[::1]:456/" &&
	test-url-normalize "xyz://[::1]:/" &&
	! test-url-normalize "xyz://[::1]:000/" &&
	! test-url-normalize "xyz://[::1]:0%300/" &&
	! test-url-normalize "xyz://[::1]:0x80/" &&
	! test-url-normalize "xyz://[::1]:4294967297/" &&
	! test-url-normalize "xyz://[::1]:030f/"
'

test_expect_success 'url port normalization' '
	test "$(test-url-normalize -p "http://x:800")" = "http://x:800/" &&
	test "$(test-url-normalize -p "http://x:0800")" = "http://x:800/" &&
	test "$(test-url-normalize -p "http://x:00000800")" = "http://x:800/" &&
	test "$(test-url-normalize -p "http://x:065535")" = "http://x:65535/" &&
	test "$(test-url-normalize -p "http://x:1")" = "http://x:1/" &&
	test "$(test-url-normalize -p "http://x:80")" = "http://x/" &&
	test "$(test-url-normalize -p "http://x:080")" = "http://x/" &&
	test "$(test-url-normalize -p "http://x:000000080")" = "http://x/" &&
	test "$(test-url-normalize -p "https://x:443")" = "https://x/" &&
	test "$(test-url-normalize -p "https://x:0443")" = "https://x/" &&
	test "$(test-url-normalize -p "https://x:000000443")" = "https://x/"
'

test_expect_success 'url general escapes' '
	! test-url-normalize "http://x.y?%fg" &&
	test "$(test-url-normalize -p "X://W/%7e%41^%3a")" = "x://w/~A%5E%3A" &&
	test "$(test-url-normalize -p "X://W/:/?#[]@")" = "x://w/:/?#[]@" &&
	test "$(test-url-normalize -p "X://W/$&()*+,;=")" = "x://w/$&()*+,;=" &&
	test "$(test-url-normalize -p "X://W/'\''")" = "x://w/'\''" &&
	test "$(test-url-normalize -p "X://W?'\!'")" = "x://w/?'\!'"
'

test_expect_success 'url high-bit escapes' '
	test "$(test-url-normalize -p "$(cat "$tu-1")")" = "x://q/%01%02%03%04%05%06%07%08%0E%0F%10%11%12" &&
	test "$(test-url-normalize -p "$(cat "$tu-2")")" = "x://q/%13%14%15%16%17%18%19%1B%1C%1D%1E%1F%7F" &&
	test "$(test-url-normalize -p "$(cat "$tu-3")")" = "x://q/%80%81%82%83%84%85%86%87%88%89%8A%8B%8C%8D%8E%8F" &&
	test "$(test-url-normalize -p "$(cat "$tu-4")")" = "x://q/%90%91%92%93%94%95%96%97%98%99%9A%9B%9C%9D%9E%9F" &&
	test "$(test-url-normalize -p "$(cat "$tu-5")")" = "x://q/%A0%A1%A2%A3%A4%A5%A6%A7%A8%A9%AA%AB%AC%AD%AE%AF" &&
	test "$(test-url-normalize -p "$(cat "$tu-6")")" = "x://q/%B0%B1%B2%B3%B4%B5%B6%B7%B8%B9%BA%BB%BC%BD%BE%BF" &&
	test "$(test-url-normalize -p "$(cat "$tu-7")")" = "x://q/%C0%C1%C2%C3%C4%C5%C6%C7%C8%C9%CA%CB%CC%CD%CE%CF" &&
	test "$(test-url-normalize -p "$(cat "$tu-8")")" = "x://q/%D0%D1%D2%D3%D4%D5%D6%D7%D8%D9%DA%DB%DC%DD%DE%DF" &&
	test "$(test-url-normalize -p "$(cat "$tu-9")")" = "x://q/%E0%E1%E2%E3%E4%E5%E6%E7%E8%E9%EA%EB%EC%ED%EE%EF" &&
	test "$(test-url-normalize -p "$(cat "$tu-10")")" = "x://q/%F0%F1%F2%F3%F4%F5%F6%F7%F8%F9%FA%FB%FC%FD%FE%FF" &&
	test "$(test-url-normalize -p "$(cat "$tu-11")")" = "x://q/%C2%80%DF%BF%E0%A0%80%EF%BF%BD%F0%90%80%80%F0%AF%BF%BD"
'

test_expect_success 'url username/password escapes' '
	test "$(test-url-normalize -p "x://%41%62(^):%70+d@foo")" = "x://Ab(%5E):p+d@foo/"
'

test_expect_success 'url normalized lengths' '
	test "$(test-url-normalize -l "Http://%4d%65:%4d^%70@The.Host")" = 25 &&
	test "$(test-url-normalize -l "http://%41:%42@x.y/%61/")" = 17 &&
	test "$(test-url-normalize -l "http://@x.y/^")" = 15
'

test_expect_success 'url . and .. segments' '
	test "$(test-url-normalize -p "x://y/.")" = "x://y/" &&
	test "$(test-url-normalize -p "x://y/./")" = "x://y/" &&
	test "$(test-url-normalize -p "x://y/a/.")" = "x://y/a" &&
	test "$(test-url-normalize -p "x://y/a/./")" = "x://y/a/" &&
	test "$(test-url-normalize -p "x://y/.?")" = "x://y/?" &&
	test "$(test-url-normalize -p "x://y/./?")" = "x://y/?" &&
	test "$(test-url-normalize -p "x://y/a/.?")" = "x://y/a?" &&
	test "$(test-url-normalize -p "x://y/a/./?")" = "x://y/a/?" &&
	test "$(test-url-normalize -p "x://y/a/./b/.././../c")" = "x://y/c" &&
	test "$(test-url-normalize -p "x://y/a/./b/../.././c/")" = "x://y/c/" &&
	test "$(test-url-normalize -p "x://y/a/./b/.././../c/././.././.")" = "x://y/" &&
	! test-url-normalize "x://y/a/./b/.././../c/././.././.." &&
	test "$(test-url-normalize -p "x://y/a/./?/././..")" = "x://y/a/?/././.." &&
	test "$(test-url-normalize -p "x://y/%2e/")" = "x://y/" &&
	test "$(test-url-normalize -p "x://y/%2E/")" = "x://y/" &&
	test "$(test-url-normalize -p "x://y/a/%2e./")" = "x://y/" &&
	test "$(test-url-normalize -p "x://y/b/.%2E/")" = "x://y/" &&
	test "$(test-url-normalize -p "x://y/c/%2e%2E/")" = "x://y/"
'

# http://@foo specifies an empty user name but does not specify a password
# http://foo  specifies neither a user name nor a password
# So they should not be equivalent
test_expect_success 'url equivalents' '
	test-url-normalize "httP://x" "Http://X/" &&
	test-url-normalize "Http://%4d%65:%4d^%70@The.Host" "hTTP://Me:%4D^p@the.HOST:80/" &&
	! test-url-normalize "https://@x.y/^" "httpS://x.y:443/^" &&
	test-url-normalize "https://@x.y/^" "httpS://@x.y:0443/^" &&
	test-url-normalize "https://@x.y/^/../abc" "httpS://@x.y:0443/abc" &&
	test-url-normalize "https://@x.y/^/.." "httpS://@x.y:0443/"
'

test_expect_success 'url config normalization matching' '
	test "$(test-url-normalize -c "$tc-1" "useragent" "https://other.example.com/")" = "other-agent" &&
	test "$(test-url-normalize -c "$tc-1" "useragent" "https://example.com/")" = "example-agent" &&
	test "$(test-url-normalize -c "$tc-1" "sslVerify" "https://example.com/")" = "false" &&
	test "$(test-url-normalize -c "$tc-1" "useragent" "https://example.com/path/sub")" = "path-agent" &&
	test "$(test-url-normalize -c "$tc-1" "sslVerify" "https://example.com/path/sub")" = "false" &&
	test "$(test-url-normalize -c "$tc-1" "noEPSV" "https://elsewhere.com/")" = "true" &&
	test "$(test-url-normalize -c "$tc-1" "noEPSV" "https://example.com")" = "true" &&
	test "$(test-url-normalize -c "$tc-1" "noEPSV" "https://example.com/path")" = "true" &&
	test "$(test-url-normalize -c "$tc-2" "useragent" "HTTPS://example.COM/p%61th")" = "example-agent" &&
	test "$(test-url-normalize -c "$tc-2" "sslVerify" "HTTPS://example.COM/p%61th")" = "false" &&
	test "$(test-url-normalize -c "$tc-3" "sslcainfo" "https://user@example.com/path/name/here")" = "file-1"
'

test_done
