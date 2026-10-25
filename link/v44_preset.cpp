/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    v44_preset.cpp - V.cvm's preset dictionary for V.44 (see
 *                     v44_preset.h); the Recommendation's Annex E lists
 *                     the same lines (the v44_preset_check test
 *                     compares them)
 *
 */

#include "v44_preset.h"

namespace dlink {

/* the lines, in order; each goes out with CR LF after it */
static const char *const lines[] = {
	"GET / HTTP/1.1",
	"Host: www.example.com",
	"User-Agent: Mozilla/5.0 (compatible)",
	"Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
	"Accept-Language: en-US,en;q=0.5",
	"Accept-Encoding: gzip, deflate",
	"Connection: keep-alive",
	"Cache-Control: no-cache",
	"Content-Type: application/x-www-form-urlencoded",
	"Content-Length: 0",
	"Authorization: Basic ",
	"Cookie: session=",
	"POST /api/v1/ HTTP/1.1",
	"Content-Type: application/json; charset=utf-8",
	"HTTP/1.1 200 OK",
	"HTTP/1.1 301 Moved Permanently",
	"HTTP/1.1 304 Not Modified",
	"HTTP/1.1 404 Not Found",
	"HTTP/1.1 500 Internal Server Error",
	"Date: Mon, 01 Jan 2024 00:00:00 GMT",
	"Server: Apache",
	"Last-Modified: Tue, 02 Feb 2024 12:00:00 GMT",
	"Location: https://www.example.com/index.html",
	"Set-Cookie: id=; Path=/; HttpOnly",
	"Transfer-Encoding: chunked",
	"Content-Type: text/html; charset=UTF-8",
	"",
	"<!DOCTYPE html>",
	"<html lang=\"en\">",
	"<head>",
	"<meta charset=\"utf-8\">",
	"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">",
	"<title></title>",
	"<link rel=\"stylesheet\" href=\"style.css\">",
	"<script type=\"text/javascript\" src=\"\"></script>",
	"</head>",
	"<body>",
	"<div class=\"\"><span></span></div>",
	"<p><a href=\"https://www.\">.com</a></p>",
	"<ul><li></li></ul>",
	"<table><tr><td></td></tr></table>",
	"<img src=\"\" alt=\"\"><br>",
	"<form action=\"\" method=\"post\"><input type=\"text\" name=\"\" value=\"\">",
	"</body>",
	"</html>",
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>",
	"{\"id\": 1, \"name\": \"\", \"type\": \"\", \"value\": 0, \"status\": \"ok\",",
	" \"time\": \"2024-01-01T00:00:00Z\", \"data\": [], \"error\": null,",
	" \"enabled\": true, \"count\": 0, \"message\": \"\", \"result\": false}",
	"From: <user@example.com>",
	"To: <user@example.com>",
	"Cc: ",
	"Subject: Re: ",
	"Date: Wed, 03 Jan 2024 10:00:00 +0000",
	"Message-ID: <@mail.example.com>",
	"Reply-To: ",
	"MIME-Version: 1.0",
	"Content-Type: text/plain; charset=\"us-ascii\"",
	"Content-Type: multipart/mixed; boundary=\"",
	"Content-Transfer-Encoding: 7bit",
	"Content-Disposition: attachment; filename=\"",
	"Received: from mail.example.com by mx.example.org with ESMTP id",
	"EHLO example.com",
	"250-SIZE 10240000",
	"MAIL FROM:<user@example.com>",
	"RCPT TO:<user@example.com>",
	"DATA",
	"354 Start mail input; end with <CRLF>.<CRLF>",
	"250 OK",
	"220 Service ready",
	"221 Bye",
	"QUIT",
	"+OK POP3 server ready",
	"USER ",
	"PASS ",
	"STAT",
	"LIST",
	"RETR 1",
	"DELE 1",
	"-ERR ",
	"A001 LOGIN ",
	"A002 SELECT INBOX",
	"A003 FETCH 1 (FLAGS BODY[HEADER])",
	"* OK [CAPABILITY IMAP4rev1] ready",
	"$GPGGA,000000.00,0000.0000,N,00000.0000,E,1,08,1.0,0.0,M,0.0,M,,*00",
	"$GPRMC,000000.00,A,0000.0000,N,00000.0000,E,0.0,0.0,010124,,,A*00",
	"$GPGSV,3,1,12,",
	"$GPVTG,0.0,T,,M,0.0,N,0.0,K,A*00",
	"login: ",
	"Password: ",
	"Last login: ",
	"Welcome to ",
	"Connection closed by foreign host.",
	"ERROR",
	"OK",
	"0123456789 10 100 1000 2024 00:00:00",
	"https://www.example.com/index.html?id=&page=1&lang=en",
	"http://www.example.org/ .net/ .gov/ .edu/",
	"The ",
	"the and of to in is that for it with as was on be at by this are from",
	" have or not but which you an they his her were had has their been",
	" one all there more if will can would what so out about up who when",
	" also into its other new some time only these may than then them two",
	" first any my like over such our well even most made after should",
	" could because between through during before under while where your",
	" please thank you regards information message number service report",
	" system user name address phone email account date error status",
	" data file page text total price order today tomorrow yesterday",
	"ing tion ment ness able ally ed. es, ly s. ",
	"Dear Sir or Madam,",
	"Thank you for your message. Best regards,",
	"-----BEGIN-----",
	"-----END-----",
};

/* see v44_preset.h */
const std::vector<u8> &v44_preset()
{
	static const std::vector<u8> text = [] {
		std::vector<u8> t;
		for (const char *l : lines) {
			while (*l)
				t.push_back(static_cast<u8>(*l++));
			t.push_back('\r');
			t.push_back('\n');
		}
		return t;
	}();
	return text;
}

} // namespace dlink
