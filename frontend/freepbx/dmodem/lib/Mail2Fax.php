<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// lib/Mail2Fax.php - email to fax: a message to <number>@<the fax mail
// domain> becomes a fax to that number (its attached files, and its
// text on the cover page).  This file has the rules (the numbers of the
// addresses, the senders, the PIN) and the job of each number; the
// spool and the queue are in Dmodem.class.php (mail2fax(),
// mailSpool()), and bin/mail2fax-spool.php puts the messages from the
// mail system into the spool.

namespace FreePBX\modules\Dmodem;

class Mail2Fax
{
	/* the most bytes of a message, and the most numbers of one message */
	const MAX_BYTES = 31457280;
	const MAX_NUMBERS = 10;
	/* the most lines of the senders list */
	const MAX_SENDERS = 500;
	/* the headers whose addresses can give the numbers, in order */
	const TO_HEADERS = ['x-original-to', 'delivered-to', 'to', 'cc'];

	/* the fax number of address a, null if it has none: the local part
	   (after its last +, as in fax+5550100@...), with the characters
	   - . ( ) removed; only in domain d when d is not '' */
	public static function number($a, $d = '')
	{
		$a = strtolower(trim((string)$a));
		$at = strrpos($a, '@');
		if ($at === false || $at === 0)
			return null;
		if ($d !== '' && substr($a, $at + 1) !== strtolower($d))
			return null;
		$local = substr($a, 0, $at);
		$plus = strrpos($local, '+');
		if ($plus !== false && $plus > 0)
			$local = substr($local, $plus + 1);
		$n = str_replace(['-', '.', '(', ')'], '', $local);
		return preg_match('/^\+?[0-9*#]{1,40}$/', $n) ? $n : null;
	}

	/* the lines of senders list t: addresses and @domains in lower
	   case (comments after # and empty lines left out) */
	public static function senders($t)
	{
		$out = [];
		foreach (preg_split('/\r\n|\r|\n/', (string)$t) as $l) {
			$p = strpos($l, '#');
			if ($p !== false)
				$l = substr($l, 0, $p);
			$l = strtolower(trim($l));
			if ($l !== '')
				$out[] = $l;
		}
		return $out;
	}

	/* what is wrong with senders list t, '' if nothing */
	public static function sendersError($t)
	{
		$lines = self::senders($t);
		if (count($lines) > self::MAX_SENDERS)
			return sprintf(_('%d senders at most'), self::MAX_SENDERS);
		foreach ($lines as $l)
			if (!preg_match('/^([A-Za-z0-9.!#$%&\'*+\/=?^_`{|}~-]+)?@[a-z0-9.-]+\.[a-z]{2,}$/', $l))
				return sprintf(_('Not an address or an @domain: %s'), $l);
		return '';
	}

	/* true if address a is on senders list t (the address, or its
	   @domain) */
	public static function senderOk($a, $t)
	{
		$a = strtolower(trim((string)$a));
		$at = strrpos($a, '@');
		if ($at === false)
			return false;
		$dom = substr($a, $at);
		foreach (self::senders($t) as $l)
			if ($l === $a || $l === $dom)
				return true;
		return false;
	}

	/* true if subject s has pin p as a word (p '': no PIN asked) */
	public static function pinOk($s, $p)
	{
		$p = (string)$p;
		return $p === '' ||
			preg_match('/(^|[^0-9A-Za-z])' . preg_quote($p, '/') . '([^0-9A-Za-z]|$)/', (string)$s) === 1;
	}

	/* the request of message raw with settings s: ['ok', 'why',
	   'reply' (true: the sender may get the refusal), 'from' ([name,
	   address]), 'numbers', 'docs' ([[name, bytes]]: PDF, TIFF and
	   Office files), 'skipped' (the names of the other files), 'text'
	   (the message's text), 'subject'] */
	public static function read($raw, array $s)
	{
		$r = ['ok' => false, 'why' => '', 'reply' => false, 'from' => ['', ''],
		      'numbers' => [], 'docs' => [], 'skipped' => [], 'text' => '', 'subject' => ''];
		if (strlen((string)$raw) > self::MAX_BYTES) {
			$r['why'] = _('The message is too big');
			return $r;
		}
		list($h) = MailParse::split($raw);
		$from = MailParse::addresses(MailParse::header($h, 'from'));
		if (!$from) {
			$r['why'] = _('The message has no sender');
			return $r;
		}
		$r['from'] = $from[0];
		$r['subject'] = trim(MailParse::decodeWords(MailParse::header($h, 'subject')));
		/* no answers to automatic messages (no mail loops) */
		$auto = strtolower(MailParse::header($h, 'auto-submitted'));
		$prec = strtolower(MailParse::header($h, 'precedence'));
		if (($auto !== '' && $auto !== 'no') || in_array($prec, ['bulk', 'junk', 'list'], true) ||
		    MailParse::header($h, 'list-id') !== '') {
			$r['why'] = _('An automatic or list message');
			return $r;
		}
		if (!self::senderOk($r['from'][1], $s['mail2fax_senders'])) {
			$r['why'] = sprintf(_('The sender %s is not on the senders list'), $r['from'][1]);
			return $r;
		}
		if (!self::pinOk($r['subject'], $s['mail2fax_pin'])) {
			$r['why'] = _('The subject does not have the PIN');
			return $r;
		}
		/* from here, the sender is known: it gets the refusal */
		$r['reply'] = true;
		foreach (self::TO_HEADERS as $k) {
			foreach (isset($h[$k]) ? $h[$k] : [] as $v)
				foreach (MailParse::addresses($v) as $a) {
					$n = self::number($a[1], $s['mail2fax_domain']);
					if ($n !== null && !in_array($n, $r['numbers'], true))
						$r['numbers'][] = $n;
				}
			/* the original recipient comes first: the other headers
			   are only for a message without it */
			if ($r['numbers'] && ($k === 'x-original-to' || $k === 'delivered-to'))
				break;
		}
		if (!$r['numbers']) {
			$r['why'] = _('No fax number in the recipient address (<number>@<domain>)');
			return $r;
		}
		if (count($r['numbers']) > self::MAX_NUMBERS) {
			$r['why'] = sprintf(_('%d fax numbers at most'), self::MAX_NUMBERS);
			return $r;
		}
		$parts = MailParse::parts($raw);
		$r['text'] = MailParse::text($parts);
		foreach (MailParse::files($parts) as $f) {
			if (FaxSend::docKind($f[0], $f[1]) !== null)
				$r['docs'][] = $f;
			else
				$r['skipped'][] = $f[0];
		}
		if (!$r['docs'] && !$s['mail2fax_cover']) {
			$r['why'] = _('No PDF, TIFF or Office file is attached');
			return $r;
		}
		$r['ok'] = true;
		return $r;
	}

	/* the job (the input of Dmodem::queueFax()) of number n of request
	   r (read()) with settings s: the cover page has the sender and the
	   message's text, and the result goes to the sender */
	public static function job(array $r, $n, array $s)
	{
		$cover = $s['mail2fax_cover'] && ($r['text'] !== '' || !$r['docs']);
		$from = $r['from'][0] !== '' ? $r['from'][0] : $r['from'][1];
		$text = $r['text'];
		if ($s['mail2fax_pin'] === '' && $r['subject'] !== '')
			$text = $r['subject'] . ($text !== '' ? "\n\n" . $text : '');
		return [
			'number' => $n,
			'cover' => $cover ? 'yes' : 'no',
			'to' => '',
			'from' => self::cut(preg_replace('/[\x00-\x1f]/', ' ', $from), 100),
			'message' => self::cut(str_replace("\0", '', $text), 3000),
			'notify' => $s['fax_notify'],
			'email' => $r['from'][1],
		];
	}

	/* text t cut to max bytes, on a whole UTF-8 character */
	private static function cut($t, $max)
	{
		$t = (string)$t;
		if (strlen($t) <= $max)
			return $t;
		$t = substr($t, 0, $max);
		/* no half character at the end */
		return preg_match('//u', $t) ? $t : preg_replace('/[\xc0-\xff][\x80-\xbf]*$/', '', $t);
	}
}
