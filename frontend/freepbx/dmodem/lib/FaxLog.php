<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// lib/FaxLog.php - the fax log: one entry for each received fax (the
// hangup handler of Dialplan::mailHandler() through fwconsole dmodem
// fax2mail), kept by the module (Dmodem::faxReceived()).  This file has
// the entry's form and the checks; the store is in Dmodem.class.php.

namespace FreePBX\modules\Dmodem;

class FaxLog
{
	/* the call results that an entry keeps, as the hangup handler
	   gives them (fwconsole dmodem fax2mail's arguments 3 to 8) */
	const VARS = ['status', 'pages', 'cid', 'remote', 'cidname', 'rate'];
	/* the most entries kept: the oldest go first */
	const MAX_ENTRIES = 5000;

	/* true if u is a call's uniqueid (Asterisk: <seconds>.<n>) */
	public static function uniqueidOk($u)
	{
		return preg_match('/^[0-9]{1,20}(\.[0-9]{1,10})?$/', (string)$u) === 1;
	}

	/* the log entry of the fax of call uniqueid to recipient id (''
	   for none), the call's results in vars (VARS), its file (a path,
	   null if there is none) and the time now (seconds); out: a sent
	   fax (['number', 'job', 'attempts', 'user' (its User Manager
	   user id, 0 for none)]) */
	public static function entry($id, $uniqueid, array $vars, $file, $now,
				     ?array $out = null)
	{
		$e = [
			'uniqueid' => (string)$uniqueid,
			'time' => (int)$now,
			'direction' => $out === null ? 'in' : 'out',
			'recipient' => (string)$id,
			'number' => $out === null ? '' : self::clean($out['number']),
			'job' => $out === null ? '' : self::clean($out['job']),
			'attempts' => $out === null ? 0 : (int)$out['attempts'],
			'user' => $out === null || !isset($out['user']) ? 0 : (int)$out['user'],
			'file' => $file === null ? '' : basename($file),
			'mail' => '',
			'mail_ok' => false,
			/* junk fax control (FaxJunk): not mailed, not in UCP */
			'blocked' => false,
		];
		foreach (self::VARS as $k)
			$e[$k] = isset($vars[$k]) ? self::clean($vars[$k]) : '';
		$e['pages'] = (int)$e['pages'];
		$e['rate'] = (int)$e['rate'];
		return $e;
	}

	/* v as an entry keeps it: one line of printable characters, 80 at
	   most */
	private static function clean($v)
	{
		return substr(preg_replace('/[^\x20-\x7e]/', '', (string)$v), 0, 80);
	}

	/* entry e of the store in the form of entry() (old or broken
	   values made safe), null if it is not an entry */
	public static function normalize($e)
	{
		if (!is_array($e) || !isset($e['uniqueid']) ||
		    !self::uniqueidOk($e['uniqueid']))
			return null;
		$sent = isset($e['direction']) && $e['direction'] === 'out' ? [
			'number' => isset($e['number']) ? $e['number'] : '',
			'job' => isset($e['job']) ? $e['job'] : '',
			'attempts' => isset($e['attempts']) ? $e['attempts'] : 0,
			'user' => isset($e['user']) ? $e['user'] : 0] : null;
		$out = self::entry(isset($e['recipient']) ? $e['recipient'] : '',
				   $e['uniqueid'], $e, null,
				   isset($e['time']) ? $e['time'] : 0, $sent);
		$out['file'] = isset($e['file']) ? self::fileName($e['file']) : '';
		$out['mail'] = isset($e['mail']) ? substr((string)$e['mail'], 0, 300) : '';
		$out['mail_ok'] = !empty($e['mail_ok']);
		$out['blocked'] = $out['direction'] === 'in' && !empty($e['blocked']);
		return $out;
	}

	/* f if it is the name of a fax file in fax_dir (no path, a .tif
	   name), else '' */
	public static function fileName($f)
	{
		$f = (string)$f;
		return preg_match('/^[A-Za-z0-9+._-]{1,200}\.tif$/', $f) &&
			strpos($f, '..') === false ? $f : '';
	}

	/* the uniqueids of the entries (uniqueid => entry) to drop: those
	   older than days (0: none for age) at the time now, then the
	   oldest above MAX_ENTRIES */
	public static function expired(array $entries, $days, $now)
	{
		uasort($entries, function ($a, $b) {
			return $b['time'] <=> $a['time'];
		});
		$out = [];
		$n = 0;
		foreach ($entries as $u => $e) {
			$n++;
			if (($days > 0 && $e['time'] < $now - $days * 86400) ||
			    $n > self::MAX_ENTRIES)
				$out[] = (string)$u;
		}
		return $out;
	}

	/* the status of entry e for the list: BLOCKED for a blocked fax,
	   else OK, FAILED and others as the engine gave them, "?" if none */
	public static function status(array $e)
	{
		if (!empty($e['blocked']))
			return 'BLOCKED';
		return $e['status'] !== '' ? $e['status'] : '?';
	}
}
