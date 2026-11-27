<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// Dialplan.php - the module's context, app-dmodem: an extension for each
// profile (the destination app-dmodem,<profile>,1), receivefax, the fax
// destination, and fax-<id> for each fax recipient (the fax received,
// then mailed by the hangup handler, app-dmodem-fax2mail).  build() gives the steps; Dmodem::doDialplanHook()
// hands them to FreePBX, render() writes them as extensions.conf would
// (for the tests and the page that shows them).

namespace FreePBX\modules\Dmodem;

class Dialplan
{
	const CONTEXT = 'app-dmodem';
	/* the fax destination's extension */
	const FAX_EXTEN = 'receivefax';
	/* a fax recipient's extension: this and its id */
	const RECIPIENT_PREFIX = 'fax-';
	/* the hangup handler's context: the fax to email */
	const MAIL_CONTEXT = 'app-dmodem-fax2mail';

	/* the received fax's file name, in fax_dir: as app_dmodem names
	   the faxes it takes by itself, <date>-<time>-<caller>-<uniqueid> */
	public static function faxFile($dir)
	{
		return rtrim($dir, '/') .
			'/${STRFTIME(${EPOCH},,%Y%m%d-%H%M%S)}' .
			'-${FILTER(0-9A-Za-z+,${CALLERID(num)})}-${UNIQUEID}.tif';
	}

	/* the steps that receive a fax into fax_dir (s the settings) */
	private static function receive(array $s, $label)
	{
		$file = self::faxFile($s['fax_dir']);
		return [
			['NoOp', $label],
			$s['engine'] === 'ari' ?
				['Stasis', $s['ari_app'] . ',receivefax,' . $file] :
				['DModemReceiveFax', $file],
			['NoOp', 'D-Modem fax: ${DMODEMFAXSTATUS} ' .
			 '${DMODEMFAXPAGES} pages ${DMODEMFAXFILE}'],
			['Hangup', ''],
		];
	}

	/* the hangup handler (MAIL_CONTEXT, s): fwconsole (its path) dmodem
	   fax2mail with the recipient's id (ARG1) and the call's results.
	   Each value is filtered to the characters it can have, inside
	   '...', so that nothing of the call reaches the shell as such */
	public static function mailHandler($fwconsole)
	{
		$q = function ($v) {
			return "'" . $v . "'";
		};
		return [
			['System', $fwconsole . ' dmodem fax2mail ' . implode(' ', [
				$q('${FILTER(a-z0-9_,${ARG1})}'),
				$q('${FILTER(0-9.,${UNIQUEID})}'),
				$q('${FILTER(A-Z_,${DMODEMFAXSTATUS})}'),
				$q('${FILTER(0-9,${DMODEMFAXPAGES})}'),
				$q('${FILTER(0-9+,${CALLERID(num)})}'),
				$q('${FILTER(0-9+ ,${DMODEMFAXREMOTE})}'),
				$q('${FILTER(A-Za-z0-9 .,${CALLERID(name)})}'),
			])],
			['Return', ''],
		];
	}

	/* the context: exten => [[application, data], ...] in order;
	   recipients: id => recipient */
	public static function build(array $settings, array $profiles,
				     array $recipients = [])
	{
		$s = array_merge(Config::defaultSettings(), $settings);
		$ari = $s['engine'] === 'ari';
		$x = [];
		ksort($profiles, SORT_STRING);
		foreach ($profiles as $name => $p) {
			$x[$name] = [
				['NoOp', 'D-Modem profile ' . $name],
				$ari ? ['Stasis', $s['ari_app'] . ',' . $name]
				     : ['DModem', $name],
				['NoOp', 'D-Modem ' . $name .
				 ': ${DMODEMSTATUS} ${DMODEMRATE}'],
				['Hangup', ''],
			];
		}
		if ($s['fax_destination'])
			$x[self::FAX_EXTEN] = self::receive($s, 'D-Modem fax');
		ksort($recipients, SORT_STRING);
		foreach ($recipients as $id => $r) {
			$steps = self::receive($s, 'D-Modem fax to ' . $id);
			array_splice($steps, 1, 0, [['Set',
				'CHANNEL(hangup_handler_push)=' . self::MAIL_CONTEXT .
				',s,1(' . $id . ')']]);
			$x[self::RECIPIENT_PREFIX . $id] = $steps;
		}
		return $x;
	}

	/* the contexts where FreePBX's inbound routes are (Core) */
	const DID_CONTEXTS = ['ext-did-0001', 'ext-did-0002'];
	/* the label in a route's extension before which the fax detection
	   goes (Core's "CallerID Entry Point") */
	const DID_HOOK = 'did-cid-hook';

	/* the extension of the inbound route did/cid in ext-did-000x, as
	   Core names it: <did>[/<cid>], s for no DID, s/<cid> for a route
	   of the caller ID alone */
	public static function didExten($did, $cid)
	{
		if ($did === '' && $cid !== '')
			return 's/' . $cid;
		return ($did === '' ? 's' : $did) . ($cid === '' ? '' : '/' . $cid);
	}

	/* the steps of the fax detection f of an inbound route (s the
	   settings): the engine listens (option x: it does not go to a fax
	   extension), then a fax goes to f's destination; no fax: the route
	   goes on */
	public static function didFax(array $s, array $f)
	{
		$o = 'x' . ($f['ring'] ? 'r' : '') .
			($f['listen'] === 'cng' ? 'c' : ($f['listen'] === 't38' ? 't' : ''));
		return [
			$s['engine'] === 'ari' ?
				['Stasis', $s['ari_app'] . ',faxdetect,' . $f['wait'] . ',' . $o] :
				['DModemFaxDetect', $f['wait'] . ',' . $o],
			['GotoIf', '$["${DMODEMFAXDETECTED}" = "CNG" | ' .
			 '"${DMODEMFAXDETECTED}" = "T38"]?' . $f['dest']],
		];
	}

	/* the context as extensions.conf text (name: the context's) */
	public static function render(array $context, $name = self::CONTEXT)
	{
		$o = ['[' . $name . ']'];
		foreach ($context as $exten => $steps) {
			foreach ($steps as $i => $st)
				$o[] = ($i === 0 ? 'exten => ' . $exten . ',1,'
					    : ' same => n,') .
					$st[0] . '(' . $st[1] . ')';
		}
		return implode("\n", $o) . "\n";
	}
}
