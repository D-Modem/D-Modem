<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// gen.php <dir> [app|ari] [ari port]: what the FreePBX module writes on
// Apply Config, made the way the page makes it (its forms posted through
// doConfigPageInit()): <dir>/dmodem.conf and <dir>/extensions_dmodem.conf
// (the app-dmodem context, as FreePBX writes extensions_additional.conf).
// The profiles: bbs (TCP 127.0.0.1:2323, sip_call.py's BBS) and term
// (127.0.0.1:2324, its terminal, for calls out), the fax destination,
// calls out dialled in modem-out.  dmodem_test_module() is the same
// module for the other scripts (dial.php).

require_once __DIR__ . '/fpbx_env.php';

/* the module as e2e.sh has it set up (dir: where faxes go) */
function dmodem_test_module($dir, $engine, $port)
{
	$m = dmodem_new();
	dmodem_post($m, ['action' => 'save_settings', 'engine' => $engine, 'debug' => '1',
			 'fax_dir' => $dir . '/fax', 'fax_max_rate' => '14400',
			 'fax_ecm' => 'yes', 'fax_t38' => 'yes', 'fax_destination' => 'yes',
			 'ari_url' => 'http://127.0.0.1:' . $port, 'ari_user' => 'dmodem',
			 'ari_password' => 'dmodem', 'ari_app' => 'dmodem',
			 'sip_endpoint' => 'dmodem', 'sip_bind' => '127.0.0.1:5090',
			 'dialout_context' => 'modem-out', 'dialout_callerid' => '"D-Modem" <5550100>',
			 'dialout_ring' => '30']);
	$p = ['action' => 'save_profile', 'original' => '', 'mode' => 'tcp',
	      'host' => '127.0.0.1', 'law' => 'auto', 'timeout' => '60', 'fax' => 'yes'];
	dmodem_post($m, $p + ['name' => 'bbs', 'description' => 'the test BBS', 'port' => '2323']);
	dmodem_post($m, $p + ['name' => 'term', 'description' => 'the test terminal', 'port' => '2324']);
	if ($m->errors) {
		fwrite(STDERR, 'gen.php: ' . json_encode($m->errors) . "\n");
		exit(1);
	}
	return $m;
}

if (realpath($argv[0]) === __FILE__) {
	$dir = isset($argv[1]) ? $argv[1] : '.';
	$engine = isset($argv[2]) ? $argv[2] : 'app';
	$m = dmodem_test_module($dir, $engine, isset($argv[3]) ? $argv[3] : '18088');
	$m->writeConfig($m->genConfig());
	file_put_contents($dir . '/dmodem.conf', $m->FreePBX->written['dmodem.conf']);
	file_put_contents($dir . '/extensions_dmodem.conf', dmodem_dialplan($m));
	@mkdir($dir . '/fax');
	echo "gen.php: dmodem.conf and extensions_dmodem.conf ($engine) in $dir\n";
}
