<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// lib/FaxJunk.php - junk fax control: the block list and the allow list
// of caller IDs (the settings fax_block, fax_allow and fax_allow_only).
// A received fax from a blocked caller ID goes into the fax log as
// BLOCKED, and it does not go by email (Dmodem::faxReceived()).

namespace FreePBX\modules\Dmodem;

class FaxJunk
{
	/* the most lines of a list, and the most characters of a line */
	const MAX_LINES = 500;
	const MAX_LINE = 40;
	/* the line that matches a call without a caller ID number */
	const ANONYMOUS = 'anonymous';

	/* the digits of caller ID cid ('' for none: anonymous, restricted
	   and similar) */
	public static function digits($cid)
	{
		return preg_replace('/[^0-9]/', '', (string)$cid);
	}

	/* the entries of list text t: one for each line, with the text
	   after # (a comment) and the spaces removed, empty lines left out */
	public static function parse($t)
	{
		$out = [];
		foreach (preg_split('/\r\n|\r|\n/', (string)$t) as $l) {
			$p = strpos($l, '#');
			if ($p !== false)
				$l = substr($l, 0, $p);
			$l = strtolower(preg_replace('/\s+/', '', $l));
			if ($l !== '')
				$out[] = $l;
		}
		return $out;
	}

	/* true if entry e is good: digits (and + - ( ) . ignored), with an
	   optional * at the end for all numbers that start with them, or
	   "anonymous" */
	public static function entryOk($e)
	{
		return $e === self::ANONYMOUS ||
			(strlen($e) <= self::MAX_LINE &&
			 preg_match('/^\+?[0-9().-]*[0-9][0-9().-]*\*?$/', $e) === 1);
	}

	/* what is wrong with list text t, '' if nothing */
	public static function listError($t)
	{
		$t = (string)$t;
		if (strpos($t, "\0") !== false)
			return _('No NUL characters');
		$lines = self::parse($t);
		if (count($lines) > self::MAX_LINES)
			return sprintf(_('%d numbers at most'), self::MAX_LINES);
		foreach ($lines as $e)
			if (!self::entryOk($e))
				return sprintf(_('Not a number, a prefix with * or anonymous: %s'), $e);
		return '';
	}

	/* true if caller ID cid matches entry e of a list */
	public static function matches($cid, $e)
	{
		$d = self::digits($cid);
		if ($e === self::ANONYMOUS)
			return $d === '';
		if ($d === '')
			return false;
		$prefix = substr($e, -1) === '*';
		$n = self::digits($e);
		return $prefix ? strncmp($d, $n, strlen($n)) === 0 : $d === $n;
	}

	/* true if caller ID cid matches an entry of list text t */
	public static function listed($cid, $t)
	{
		foreach (self::parse($t) as $e)
			if (self::entryOk($e) && self::matches($cid, $e))
				return true;
		return false;
	}

	/* why a fax from caller ID cid is blocked with settings s, '' if
	   it is not.  The allow list comes first: a caller ID on it is
	   never blocked */
	public static function blocked($cid, array $s)
	{
		if (self::listed($cid, $s['fax_allow']))
			return '';
		if (self::listed($cid, $s['fax_block']))
			return _('On the block list');
		if ($s['fax_allow_only'])
			return _('Not on the allow list');
		return '';
	}

	/* list text t with caller ID cid added at the end (no change if
	   cid has no digits or is on it already) */
	public static function add($t, $cid)
	{
		$d = self::digits($cid);
		if ($d === '' || in_array($d, array_map([self::class, 'digits'],
		    array_filter(self::parse($t), function ($e) {
			    return substr($e, -1) !== '*' && $e !== self::ANONYMOUS;
		    })), true))
			return (string)$t;
		$t = rtrim((string)$t);
		return ($t === '' ? '' : $t . "\n") . $d;
	}
}
