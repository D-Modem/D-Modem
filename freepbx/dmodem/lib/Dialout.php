<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// Dialout.php - a modem call out: the number dialled through FreePBX's
// outbound routes (a Local channel into the dial-out context, from-internal
// by default), the modem the caller once the far end answers:
// DModem(<profile>,o), or dmodem-ari's Stasis(<app>,<profile>,o).  The
// profile's server or program then talks to the remote BBS or ISP.  The
// manager's Originate action places the call; Dmodem::dialOut() sends it.

namespace FreePBX\modules\Dmodem;

class Dialout
{
	/* true if cid is a caller ID Originate takes: empty, a number, or
	   "Name" <number> (no line breaks, nothing the manager splits on) */
	public static function callerIdOk($cid)
	{
		$cid = (string)$cid;
		return $cid === '' ||
			preg_match('/^[0-9*#+]{1,40}$/', $cid) === 1 ||
			preg_match('/^"[^"\x00-\x1f]{1,40}" <[0-9*#+]{1,40}>$/', $cid) === 1;
	}

	/* what is wrong with dialling number on profile with caller ID cid,
	   field => message; profiles: the profiles there are */
	public static function validate($profile, $number, $cid, array $profiles)
	{
		$e = [];
		if (!isset($profiles[$profile]))
			$e['profile'] = _('No such profile');
		if (!preg_match('/^[0-9*#+]{1,40}$/', (string)$number))
			$e['number'] = _('The number to dial: digits, *, # and +');
		if (!self::callerIdOk($cid))
			$e['callerid'] = _('A number, or "Name" <number>');
		return $e;
	}

	/* the manager's Originate action for the call: its fields (a checked
	   profile, number and caller ID; cid empty: the settings', if any) */
	public static function originate(array $settings, $profile, $number, $cid = '')
	{
		$s = array_merge(Config::defaultSettings(), $settings);
		$ari = $s['engine'] === 'ari';
		$o = [
			'Channel' => 'Local/' . $number . '@' . $s['dialout_context'],
			'Application' => $ari ? 'Stasis' : 'DModem',
			'Data' => ($ari ? $s['ari_app'] . ',' : '') . $profile . ',o',
			'Timeout' => (string)((int)$s['dialout_ring'] * 1000),
			'Async' => 'true',
			'Variable' => 'DMODEM_DIALOUT=' . $profile,
		];
		if ($cid === '')
			$cid = $s['dialout_callerid'];
		if ($cid !== '')
			$o['CallerID'] = $cid;
		return $o;
	}
}
