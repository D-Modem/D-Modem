<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// FaxMail.php - fax-to-email: a received fax to a recipient's address.
// The hangup handler of a recipient's destination (Dialplan) runs
// "fwconsole dmodem fax2mail" (Cli), which comes here: the fax file
// found by the call's unique ID, the subject and the text made from the
// settings' templates, the attachments (PDF, TIFF or both: FaxPdf), and
// the message (MIME, made here) given to the mail system.

namespace FreePBX\modules\Dmodem;

require_once __DIR__ . '/FaxPdf.php';

class FaxMail
{
	/* what goes with the message (the settings' fax_attach, or a
	   recipient's own) */
	const ATTACH = ['pdf', 'tiff', 'both'];
	/* the template fields */
	const FIELDS = ['cid', 'cidname', 'remote', 'pages', 'status', 'date',
			'recipient', 'file'];

	/* the template t with each {field} of vars (FIELDS) */
	public static function fill($t, array $vars)
	{
		$r = [];
		foreach (self::FIELDS as $f)
			$r['{' . $f . '}'] = isset($vars[$f]) ? (string)$vars[$f] : '';
		return strtr($t, $r);
	}

	/* true if a is one email address (no name, no line break) */
	public static function addressOk($a)
	{
		return is_string($a) && strlen($a) <= 254 &&
			preg_match('/^[A-Za-z0-9._%+\'-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}$/', $a) === 1;
	}

	/* the fax file of the call uniqueid in dir: <...>-<uniqueid>.tif
	   (Dialplan::faxFile()); null if there is none */
	public static function findFile($dir, $uniqueid)
	{
		if (!preg_match('/^[0-9]+(\.[0-9]+)?$/', (string)$uniqueid))
			return null;
		$m = glob(rtrim($dir, '/') . '/*-' . $uniqueid . '.tif');
		return $m ? $m[0] : null;
	}

	/* a header value encoded for the message (RFC 2047) when it is not
	   plain ASCII; line breaks gone */
	private static function header($v)
	{
		$v = preg_replace('/[\r\n]+/', ' ', (string)$v);
		if (preg_match('/^[\x20-\x7e]*$/', $v))
			return $v;
		return '=?UTF-8?B?' . base64_encode($v) . '?=';
	}

	/* the message: [headers (a string, lines \r\n), body] for mail();
	   attachments: a list of [file name, MIME type, bytes]; boundary:
	   the MIME boundary (a random one if null) */
	public static function build($from, $subject, $text, array $attachments,
				     $boundary = null)
	{
		if ($boundary === null)
			$boundary = 'dmodem-' . bin2hex(random_bytes(12));
		$h = [];
		if ($from !== '')
			$h[] = 'From: ' . self::header($from);
		$h[] = 'MIME-Version: 1.0';
		$h[] = 'Content-Type: multipart/mixed; boundary="' . $boundary . '"';
		$h[] = 'X-Mailer: D-Modem fax';
		$b = 'This is a message in MIME format.' . "\r\n\r\n";
		$b .= '--' . $boundary . "\r\n";
		$b .= 'Content-Type: text/plain; charset=UTF-8' . "\r\n";
		$b .= 'Content-Transfer-Encoding: base64' . "\r\n\r\n";
		$b .= chunk_split(base64_encode($text), 76, "\r\n");
		foreach ($attachments as $a) {
			list($name, $type, $bytes) = $a;
			$name = preg_replace('/[^A-Za-z0-9._+-]/', '_', $name);
			$b .= '--' . $boundary . "\r\n";
			$b .= 'Content-Type: ' . $type . '; name="' . $name . '"' . "\r\n";
			$b .= 'Content-Transfer-Encoding: base64' . "\r\n";
			$b .= 'Content-Disposition: attachment; filename="' . $name . '"' . "\r\n\r\n";
			$b .= chunk_split(base64_encode($bytes), 76, "\r\n");
		}
		$b .= '--' . $boundary . "--\r\n";
		return [implode("\r\n", $h), $b, self::header($subject)];
	}

	/* the attachments of the fax file (its bytes tiff) as kind (ATTACH)
	   on paper: [[name, type, bytes], ...]; a PDF that cannot be made
	   (a TIFF that FaxPdf does not take) goes as the TIFF, with the
	   reason in *why */
	public static function attachments($base, $tiff, $kind, $paper, &$why = null)
	{
		$out = [];
		$why = '';
		if ($kind === 'pdf' || $kind === 'both') {
			try {
				$out[] = [$base . '.pdf', 'application/pdf',
					  FaxPdf::fromTiff($tiff, $paper)];
			} catch (\RuntimeException $e) {
				$why = $e->getMessage();
				$kind = 'tiff';
			}
		}
		if ($kind === 'tiff' || $kind === 'both')
			$out[] = [$base . '.tif', 'image/tiff', $tiff];
		return $out;
	}

	/* the fax of call uniqueid (DMODEMFAX* in vars: status, pages, cid,
	   remote) mailed to recipient r (['email', 'attach', 'name']) with
	   settings s; send(to, subject, body, headers) the mail system
	   (mail() if null).  [ok, message] */
	public static function send(array $s, array $r, $uniqueid, array $vars,
				    ?callable $send = null)
	{
		$to = isset($r['email']) ? $r['email'] : '';
		if (!self::addressOk($to))
			return [false, sprintf(_('No email address for recipient %s'),
					       isset($r['name']) ? $r['name'] : '')];
		if (isset($vars['status']) && $vars['status'] === 'FAILED' &&
		    empty($vars['pages']))
			return [false, sprintf(_('Call %s had no fax pages'), $uniqueid)];
		$file = self::findFile($s['fax_dir'], $uniqueid);
		if ($file === null)
			return [false, sprintf(_('No fax file for call %s in %s'),
					       $uniqueid, $s['fax_dir'])];
		$tiff = @file_get_contents($file);
		if ($tiff === false)
			return [false, sprintf(_('Cannot read %s'), $file)];
		$vars['date'] = date('Y-m-d H:i', filemtime($file));
		$vars['recipient'] = isset($r['name']) ? $r['name'] : '';
		$vars['file'] = basename($file);
		$kind = isset($r['attach']) && in_array($r['attach'], self::ATTACH, true) ?
			$r['attach'] : $s['fax_attach'];
		$base = 'fax-' . preg_replace('/[^0-9+]/', '', isset($vars['cid']) ? $vars['cid'] : '');
		$att = self::attachments(rtrim($base, '-'), $tiff, $kind,
					 $s['fax_paper'], $why);
		$text = self::fill($s['fax_email_body'], $vars);
		if ($why !== '')
			$text .= "\n\n" . sprintf(_('(No PDF: %s. The fax is the TIFF file.)'), $why);
		list($headers, $body, $subject) = self::build(
			$s['fax_email_from'], self::fill($s['fax_email_subject'], $vars),
			$text, $att);
		if ($send === null)
			$send = function ($to, $subject, $body, $headers) {
				return mail($to, $subject, $body, $headers);
			};
		$ok = (bool)$send($to, $subject, $body, $headers);
		return [$ok, $ok ? sprintf(_('Fax %s mailed to %s'), basename($file), $to) :
			sprintf(_('The mail system did not take the fax for %s'), $to)];
	}
}
