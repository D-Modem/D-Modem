<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// LineCheck.php - the PJSIP endpoints' settings that matter to a modem
// call, from Asterisk's CLI ("pjsip show endpoints", "pjsip show endpoint
// <name>"), and what is wrong with them: V.90 and V.92 need G.711 of one
// law, untouched, end to end (docs/asterisk.md, "The channel").

namespace FreePBX\modules\Dmodem;

class LineCheck
{
	/* the settings read from "pjsip show endpoint" */
	const FIELDS = ['allow', 'dtmf_mode', 'direct_media', 'fax_detect',
			't38_udptl'];

	/* the endpoints' names in "pjsip show endpoints" */
	public static function endpointNames($out)
	{
		preg_match_all('/^\s*Endpoint:\s+([^\s<]\S*)/m', (string)$out, $m);
		return array_values(array_unique($m[1]));
	}

	/* FIELDS of "pjsip show endpoint <name>", name => value; allow as
	   the list of codecs */
	public static function endpointSettings($out)
	{
		$s = [];
		$re = '/^\s*(' . implode('|', self::FIELDS) . ')\s*:\s*(.*?)\s*$/m';
		preg_match_all($re, (string)$out, $m, PREG_SET_ORDER);
		foreach ($m as $f) {
			if (isset($s[$f[1]]))
				continue;
			$s[$f[1]] = $f[2];
		}
		if (isset($s['allow'])) {
			$c = trim($s['allow'], "() \t");
			$s['allow'] = $c === '' ? [] :
				array_map('trim', explode('|', $c));
		}
		return $s;
	}

	/* true if "module show like <name>" lists the module running */
	public static function moduleRunning($out, $module)
	{
		$re = '/^' . preg_quote($module, '/') . '(\.so)?\s.*\bRunning\b/m';
		return preg_match($re, (string)$out) === 1;
	}

	/* true if "ari show apps" lists the Stasis app */
	public static function ariAppRegistered($out, $app)
	{
		return preg_match('/^\s*' . preg_quote($app, '/') . '\s*$/m',
				  (string)$out) === 1;
	}

	/* a CLI yes/no value as a bool (null: not shown) */
	private static function yes($v)
	{
		if ($v === null)
			return null;
		return in_array(strtolower($v), ['true', 'yes', '1'], true);
	}

	/* what endpoint settings s (endpointSettings()) mean for a modem
	   call: [['level' => ok|info|warn, 'text' => ...], ...] */
	public static function evaluate(array $s)
	{
		$r = [];
		$add = function ($level, $text) use (&$r) {
			$r[] = ['level' => $level, 'text' => $text];
		};
		$codecs = isset($s['allow']) ? $s['allow'] : null;
		if ($codecs === null) {
			$add('info', _('Its codecs could not be read'));
		} else {
			$laws = array_values(array_intersect($codecs, ['ulaw', 'alaw']));
			$other = array_values(array_diff($codecs, ['ulaw', 'alaw']));
			if (!$laws)
				$add('warn', _('No G.711 (ulaw or alaw): no modem call can work, V.34 and V.90 need G.711'));
			elseif ($other)
				$add('warn', sprintf(_('Other codecs allowed besides G.711 (%s): a call can be set up on one of them, or transcoded'), implode(', ', $other)));
			if (count($laws) > 1)
				$add('warn', _('Both ulaw and alaw allowed: a call can end up converted between them, which costs V.90. Allow only the PSTN\'s law'));
			if (count($laws) === 1 && !$other)
				$add('ok', sprintf(_('%s only'), $laws[0]));
		}
		if (isset($s['dtmf_mode'])) {
			$d = strtolower($s['dtmf_mode']);
			if ($d === 'inband' || strpos($d, 'auto') === 0)
				$add('warn', sprintf(_('DTMF mode %s: Asterisk looks for DTMF tones in the audio and can take modem signals away. Use RFC 4733'), $d));
		}
		if (self::yes(isset($s['fax_detect']) ? $s['fax_detect'] : null))
			$add('warn', _('Fax detection on: the modem\'s 2100 Hz answer tone sounds like a fax machine\'s, and the call can be sent to fax'));
		if (self::yes(isset($s['direct_media']) ? $s['direct_media'] : null))
			$add('info', _('Direct media on: the audio can go around Asterisk. Turn it off if modem calls bridged through Asterisk fail; DModem() calls are not affected'));
		if (self::yes(isset($s['t38_udptl']) ? $s['t38_udptl'] : null))
			$add('info', _('T.38 on: fine for D-Modem\'s faxes, which use it on purpose; a T.38 re-INVITE in a data call ends it'));
		return $r;
	}

	/* the worst level of an evaluate() result */
	public static function worst(array $r)
	{
		$rank = ['ok' => 0, 'info' => 1, 'warn' => 2];
		$w = 'ok';
		foreach ($r as $x)
			if ($rank[$x['level']] > $rank[$w])
				$w = $x['level'];
		return $w;
	}
}
