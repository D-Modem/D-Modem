<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// Cli.php - fwconsole dmodem's subcommands (Console/Dmodem.class.php
// hands them here, so they work, and are tested, without Symfony):
// dial, profiles, export, import, fax2mail (a received fax into the fax
// log, and to a recipient's email address: the hangup handler of
// Dialplan::mailHandler()), faxes (the fax log).

namespace FreePBX\modules\Dmodem;

class Cli
{
	/* subcommand args (a list) on module m, options opts (callerid,
	   keep; send: the mail system for fax2mail, as FaxMail::send() has
	   it, for the tests), its lines to say(); 0 if it went through, 1 if
	   not, 2 for bad arguments */
	public static function run($m, array $args, array $opts, callable $say)
	{
		$cmd = isset($args[0]) ? $args[0] : '';
		switch ($cmd) {
		case 'dial':
			if (count($args) !== 3) {
				$say(_('Usage: fwconsole dmodem dial <profile> <number> [--callerid=...]'));
				return 2;
			}
			$r = $m->dialOut($args[1], $args[2],
				isset($opts['callerid']) ? $opts['callerid'] : '');
			$say($r['message']);
			return $r['ok'] ? 0 : 1;
		case 'profiles':
			foreach ($m->getProfiles() as $name => $p)
				$say(sprintf('%-20s %-5s %s', $name, $p['mode'],
					     $p['description']));
			return 0;
		case 'export':
			$json = json_encode($m->exportData(),
				JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES) . "\n";
			if (count($args) < 2) {
				$say(rtrim($json));
				return 0;
			}
			if (@file_put_contents($args[1], $json) === false) {
				$say(sprintf(_('Cannot write %s'), $args[1]));
				return 1;
			}
			/* (the ARI password is in it) */
			@chmod($args[1], 0600);
			$say(sprintf(_('Exported to %s'), $args[1]));
			return 0;
		case 'import':
			if (count($args) !== 2) {
				$say(_('Usage: fwconsole dmodem import <file> [--keep]'));
				return 2;
			}
			$text = @file_get_contents($args[1]);
			$data = $text === false ? null : json_decode($text, true);
			if (!is_array($data)) {
				$say(sprintf(_('%s is not a D-Modem export'), $args[1]));
				return 1;
			}
			$r = $m->importData($data, empty($opts['keep']));
			foreach ($r['settings'] as $f => $why)
				$say(sprintf(_('setting %s not imported: %s'), $f, $why));
			foreach ($r['skipped'] as $name => $why)
				$say(sprintf(_('%s not imported: %s'), $name,
					     implode('; ', $why)));
			$say(sprintf(_('%d profiles and %d fax recipients imported; run fwconsole reload'),
				     count($r['imported']), count($r['recipients'])));
			return $r['ok'] ? 0 : 1;
		case 'fax2mail':
			if (count($args) < 3 || count($args) > 9) {
				$say(_('Usage: fwconsole dmodem fax2mail <recipient|""> <uniqueid> [status] [pages] [cid] [remote] [cidname] [rate]'));
				return 2;
			}
			$v = [];
			foreach (FaxLog::VARS as $i => $k)
				$v[$k] = isset($args[$i + 3]) ? (string)$args[$i + 3] : '';
			$r = $m->faxReceived($args[1], $args[2], $v,
				isset($opts['send']) ? $opts['send'] : null);
			$say($r[1]);
			return $r[0] ? 0 : 1;
		case 'faxes':
			foreach ($m->getFaxes() as $u => $e)
				$say(sprintf('%s  %-16s %-8s %3d  %-16s %s', date('Y-m-d H:i', $e['time']),
					     $u, FaxLog::status($e), $e['pages'], $e['cid'],
					     $e['recipient']));
			return 0;
		default:
			$say(_('Usage: fwconsole dmodem dial <profile> <number> | profiles | export [file] | import <file> | fax2mail <recipient> <uniqueid> ... | faxes'));
			return 2;
		}
	}
}
