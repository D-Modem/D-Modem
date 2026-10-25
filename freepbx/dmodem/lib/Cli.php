<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Neo-Desktop
//
// Cli.php - fwconsole dmodem's subcommands (Console/Dmodem.class.php
// hands them here, so they work, and are tested, without Symfony):
// dial, profiles, export, import.

namespace FreePBX\modules\Dmodem;

class Cli
{
	/* subcommand args (a list) on module m, options opts (callerid,
	   keep), its lines to say(); 0 if it went through, 1 if not, 2 for
	   bad arguments */
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
				$say(sprintf(_('profile %s not imported: %s'), $name,
					     implode('; ', $why)));
			$say(sprintf(_('%d profiles imported; run fwconsole reload'),
				     count($r['imported'])));
			return $r['ok'] ? 0 : 1;
		default:
			$say(_('Usage: fwconsole dmodem dial <profile> <number> | profiles | export [file] | import <file>'));
			return 2;
		}
	}
}
