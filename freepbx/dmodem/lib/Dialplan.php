<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Neo-Desktop
//
// Dialplan.php - the module's context, app-dmodem: an extension for each
// profile (the destination app-dmodem,<profile>,1) and receivefax, the
// fax destination.  build() gives the steps; Dmodem::doDialplanHook()
// hands them to FreePBX, render() writes them as extensions.conf would
// (for the tests and the page that shows them).

namespace FreePBX\modules\Dmodem;

class Dialplan
{
	const CONTEXT = 'app-dmodem';
	/* the fax destination's extension */
	const FAX_EXTEN = 'receivefax';

	/* the received fax's file name, in fax_dir: as app_dmodem names
	   the faxes it takes by itself, <date>-<time>-<caller>-<uniqueid> */
	public static function faxFile($dir)
	{
		return rtrim($dir, '/') .
			'/${STRFTIME(${EPOCH},,%Y%m%d-%H%M%S)}' .
			'-${FILTER(0-9A-Za-z+,${CALLERID(num)})}-${UNIQUEID}.tif';
	}

	/* the context: exten => [[application, data], ...] in order */
	public static function build(array $settings, array $profiles)
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
		if ($s['fax_destination']) {
			$file = self::faxFile($s['fax_dir']);
			$x[self::FAX_EXTEN] = [
				['NoOp', 'D-Modem fax'],
				$ari ? ['Stasis', $s['ari_app'] . ',receivefax,' . $file]
				     : ['DModemReceiveFax', $file],
				['NoOp', 'D-Modem fax: ${DMODEMFAXSTATUS} ' .
				 '${DMODEMFAXPAGES} pages ${DMODEMFAXFILE}'],
				['Hangup', ''],
			];
		}
		return $x;
	}

	/* the context as extensions.conf text */
	public static function render(array $context)
	{
		$o = ['[' . self::CONTEXT . ']'];
		foreach ($context as $exten => $steps) {
			foreach ($steps as $i => $st)
				$o[] = ($i === 0 ? 'exten => ' . $exten . ',1,'
					    : ' same => n,') .
					$st[0] . '(' . $st[1] . ')';
		}
		return implode("\n", $o) . "\n";
	}
}
