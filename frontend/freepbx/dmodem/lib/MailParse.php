<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// lib/MailParse.php - our own reader of an email message (RFC 5322 and
// MIME: RFC 2045 to 2047, RFC 2231): the headers, the addresses, the
// text and the attached files.  Email to fax (Mail2Fax) uses it.

namespace FreePBX\modules\Dmodem;

class MailParse
{
	/* the most parts of a message, and the most levels of multipart */
	const MAX_PARTS = 50;
	const MAX_DEPTH = 5;

	/* message raw split into its headers (headers()) and its body */
	public static function split($raw)
	{
		$raw = (string)$raw;
		if (preg_match('/\r?\n\r?\n/', $raw, $m, PREG_OFFSET_CAPTURE)) {
			$p = $m[0][1];
			return [self::headers(substr($raw, 0, $p)),
				(string)substr($raw, $p + strlen($m[0][0]))];
		}
		return [self::headers($raw), ''];
	}

	/* the header lines of text t: lower-case name => its values, in
	   order (folded lines joined) */
	public static function headers($t)
	{
		$out = [];
		$t = preg_replace('/\r?\n[ \t]+/', ' ', (string)$t);
		foreach (preg_split('/\r?\n/', $t) as $l) {
			$p = strpos($l, ':');
			if ($p === false || $p === 0)
				continue;
			$name = strtolower(trim(substr($l, 0, $p)));
			if (!preg_match('/^[!-9;-~]+$/', $name))
				continue;
			$out[$name][] = trim(substr($l, $p + 1));
		}
		return $out;
	}

	/* the first value of header name in headers h, '' if there is none */
	public static function header(array $h, $name)
	{
		$name = strtolower($name);
		return isset($h[$name][0]) ? (string)$h[$name][0] : '';
	}

	/* the encoded words (=?charset?B|Q?text?=) of header value s in
	   UTF-8 */
	public static function decodeWords($s)
	{
		/* the space between two encoded words goes */
		$s = preg_replace('/(\?=)\s+(=\?)/', '$1$2', (string)$s);
		return preg_replace_callback('/=\?([^?\s]+)\?([BbQq])\?([^?\s]*)\?=/',
			function ($m) {
				$t = strtoupper($m[2]) === 'B' ? (string)base64_decode($m[3]) :
					quoted_printable_decode(str_replace('_', ' ', $m[3]));
				return self::toUtf8($t, $m[1]);
			}, $s);
	}

	/* text t in charset cs as UTF-8 (unknown charsets: as it is) */
	public static function toUtf8($t, $cs)
	{
		$cs = strtoupper(trim((string)$cs, " \t\"'"));
		$cs = preg_replace('/\*.*$/', '', $cs);
		if ($cs === '' || $cs === 'UTF-8' || $cs === 'US-ASCII' || !function_exists('iconv'))
			return (string)$t;
		$u = @iconv($cs, 'UTF-8//IGNORE', (string)$t);
		return $u === false ? (string)$t : $u;
	}

	/* header value v with parameters (type/sub; a=b; c="d"): [the
	   value in lower case, lower-case name => decoded value].  RFC
	   2231 names (name*=charset''text, name*0=...) are joined */
	public static function params($v)
	{
		$parts = [];
		$cur = '';
		$q = false;
		$v = (string)$v;
		for ($i = 0, $n = strlen($v); $i < $n; $i++) {
			$c = $v[$i];
			if ($c === '\\' && $q && $i + 1 < $n) {
				$cur .= $v[++$i];
				continue;
			}
			if ($c === '"') {
				$q = !$q;
				continue;
			}
			if ($c === ';' && !$q) {
				$parts[] = $cur;
				$cur = '';
				continue;
			}
			$cur .= $c;
		}
		$parts[] = $cur;
		$main = strtolower(trim(array_shift($parts)));
		$out = [];
		$ext = [];
		foreach ($parts as $p) {
			$e = strpos($p, '=');
			if ($e === false)
				continue;
			$k = strtolower(trim(substr($p, 0, $e)));
			$val = trim(substr($p, $e + 1));
			if (preg_match('/^([^*]+)\*(?:([0-9]+)\*?)?$/', $k, $m)) {
				$ext[$m[1]][isset($m[2]) && $m[2] !== '' ? (int)$m[2] : 0] =
					[substr($k, -1) === '*', $val];
				continue;
			}
			$out[$k] = self::decodeWords($val);
		}
		foreach ($ext as $k => $segs) {
			ksort($segs);
			$cs = '';
			$t = '';
			foreach ($segs as $i => $s) {
				list($enc, $val) = $s;
				if ($enc && $i === 0 && preg_match("/^([^']*)'[^']*'(.*)$/s", $val, $m)) {
					$cs = $m[1];
					$val = $m[2];
				}
				$t .= $enc ? rawurldecode($val) : $val;
			}
			$out[$k] = self::toUtf8($t, $cs);
		}
		return [$main, $out];
	}

	/* the addresses of header value v (a list: commas outside quotes
	   and <>): [[name, address in lower case]] */
	public static function addresses($v)
	{
		$out = [];
		$items = [];
		$cur = '';
		$q = false;
		$a = 0;
		$v = (string)$v;
		for ($i = 0, $n = strlen($v); $i < $n; $i++) {
			$c = $v[$i];
			if ($c === '"')
				$q = !$q;
			elseif (!$q && $c === '<')
				$a++;
			elseif (!$q && $c === '>' && $a > 0)
				$a--;
			if ($c === ',' && !$q && $a === 0) {
				$items[] = $cur;
				$cur = '';
				continue;
			}
			$cur .= $c;
		}
		$items[] = $cur;
		foreach ($items as $it) {
			$it = trim($it);
			if (preg_match('/^(.*)<([^<>\s]+@[^<>\s]+)>\s*$/s', $it, $m)) {
				$name = trim(self::decodeWords(trim($m[1])), " \t\"");
				$addr = $m[2];
			} elseif (preg_match('/^([^\s<>"]+@[^\s<>"]+)(\s*\((.*)\))?$/s', $it, $m)) {
				$name = isset($m[3]) ? trim(self::decodeWords($m[3])) : '';
				$addr = $m[1];
			} else {
				continue;
			}
			$out[] = [$name, strtolower($addr)];
		}
		return $out;
	}

	/* body b decoded from its Content-Transfer-Encoding cte */
	public static function decodeBody($b, $cte)
	{
		$cte = strtolower(trim((string)$cte));
		if ($cte === 'base64')
			return (string)base64_decode(preg_replace('/[^A-Za-z0-9+\/=]/', '', (string)$b));
		if ($cte === 'quoted-printable')
			return quoted_printable_decode(str_replace("\r\n", "\n", (string)$b));
		return (string)$b;
	}

	/* the leaf parts of message raw (its multipart parts, nested):
	   a list of ['type', 'name' (the file name, '' for none),
	   'attachment' (true for Content-Disposition: attachment),
	   'charset', 'bytes' (decoded)].  depth: the level of multipart */
	public static function parts($raw, $depth = 0)
	{
		list($h, $body) = self::split($raw);
		list($type, $tp) = self::params(self::header($h, 'content-type'));
		if ($type === '')
			$type = 'text/plain';
		if (strncmp($type, 'multipart/', 10) === 0 && isset($tp['boundary']) &&
		    $tp['boundary'] !== '' && $depth < self::MAX_DEPTH) {
			$out = [];
			foreach (self::multipart($body, $tp['boundary']) as $p) {
				foreach (self::parts($p, $depth + 1) as $leaf) {
					if (count($out) >= self::MAX_PARTS)
						return $out;
					$out[] = $leaf;
				}
			}
			return $out;
		}
		list($disp, $dp) = self::params(self::header($h, 'content-disposition'));
		$name = isset($dp['filename']) ? $dp['filename'] :
			(isset($tp['name']) ? $tp['name'] : '');
		return [[
			'type' => $type,
			'name' => $name,
			'attachment' => $disp === 'attachment',
			'charset' => isset($tp['charset']) ? $tp['charset'] : '',
			'bytes' => self::decodeBody($body, self::header($h, 'content-transfer-encoding')),
		]];
	}

	/* the parts of multipart body b with boundary bd (the preamble and
	   the epilogue left out) */
	private static function multipart($b, $bd)
	{
		$lines = preg_split('/\r?\n/', (string)$b);
		$out = [];
		$cur = null;
		foreach ($lines as $l) {
			$t = rtrim($l, " \t");
			if ($t === '--' . $bd . '--') {
				if ($cur !== null)
					$out[] = implode("\n", $cur);
				return $out;
			}
			if ($t === '--' . $bd) {
				if ($cur !== null)
					$out[] = implode("\n", $cur);
				$cur = [];
				continue;
			}
			if ($cur !== null)
				$cur[] = $l;
		}
		if ($cur !== null)
			$out[] = implode("\n", $cur);
		return $out;
	}

	/* the text of the parts (parts()) of a message: the first text/plain
	   part that is not an attachment, else the first text/html part
	   without its tags; in UTF-8, with \n line ends */
	public static function text(array $parts)
	{
		foreach (['text/plain', 'text/html'] as $want)
			foreach ($parts as $p) {
				if ($p['type'] !== $want || $p['attachment'] || $p['name'] !== '')
					continue;
				$t = self::toUtf8($p['bytes'], $p['charset']);
				if ($want === 'text/html') {
					$t = preg_replace('#<(br|/p|/div|/tr|/h[1-6])\b[^>]*>#i', "\n", $t);
					$t = preg_replace('#<(script|style)\b.*?</\1\s*>#is', '', $t);
					$t = html_entity_decode(strip_tags($t), ENT_QUOTES | ENT_HTML5, 'UTF-8');
				}
				return trim(str_replace(["\r\n", "\r"], "\n", $t));
			}
		return '';
	}

	/* the attached files of the parts (parts()) of a message: [[name,
	   bytes]]; a part is a file if it has a name, or if it is an
	   attachment, or if it is not text.  A file without a name gets
	   one from its type */
	public static function files(array $parts)
	{
		$ext = ['application/pdf' => 'pdf', 'image/tiff' => 'tif', 'image/tif' => 'tif'];
		$out = [];
		foreach ($parts as $i => $p) {
			$text = strncmp($p['type'], 'text/', 5) === 0;
			if ($p['name'] === '' && !$p['attachment'] && $text)
				continue;
			if ($p['name'] === '' && !isset($ext[$p['type']]) && !$p['attachment'])
				continue;
			$name = basename(str_replace('\\', '/', $p['name']));
			if ($name === '' || $name === '.' || $name === '..')
				$name = sprintf('attachment-%d.%s', $i + 1,
						isset($ext[$p['type']]) ? $ext[$p['type']] : 'bin');
			$out[] = [$name, $p['bytes']];
		}
		return $out;
	}
}
