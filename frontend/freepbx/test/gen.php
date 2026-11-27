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
// calls out dialled in modem-out, the fax recipient sales (fax to email)
// with <dir>/bin/fwconsole for its mail handler (fax2mail.php).
// dmodem_test_module() is the same module for the other scripts
// (dial.php, fax2mail.php).  extensions_dmodem.conf also has an inbound
// route 5551002 (ext-did-0001, a part of what Core writes) with the
// module's fax detection.

require_once __DIR__ . '/fpbx_env.php';
require_once __DIR__ . '/../dmodem/lib/Apps.php';

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
	/* a fax recipient; fwconsole (the mail handler's) in <dir>/bin */
	$cfg = new FakeConfig();
	$cfg->values['AMPSBIN'] = $dir . '/bin';
	FreePBX::$modules['Config'] = $cfg;
	if (!$m->errors)
		dmodem_post($m, ['action' => 'save_recipient', 'original' => '', 'id' => 'sales',
				 'name' => 'Sales', 'email' => 'sales@example.com', 'user' => '',
				 'attach' => 'both']);
	if ($m->errors) {
		fwrite(STDERR, 'gen.php: ' . json_encode($m->errors) . "\n");
		exit(1);
	}
	return $m;
}

/* the inbound route form r posted (Core's page): the module's part */
function dmodem_did_post_gen($m, array $r)
{
	$_REQUEST = $_POST = $r + ['display' => 'did'];
	$m->doConfigPageInit('did');
}

if (realpath($argv[0]) === __FILE__) {
	$dir = isset($argv[1]) ? $argv[1] : '.';
	$engine = isset($argv[2]) ? $argv[2] : 'app';
	$m = dmodem_test_module($dir, $engine, isset($argv[3]) ? $argv[3] : '18088');
	$m->writeConfig($m->genConfig());
	file_put_contents($dir . '/dmodem.conf', $m->FreePBX->written['dmodem.conf']);
	/* an inbound route 5551002 as Core writes it (a part), with the
	   fax detection of its D-Modem fax tab: faxes to the recipient,
	   other calls to the profile bbs */
	dmodem_did_post_gen($m, ['action' => 'addIncoming', 'extension' => '5551002',
		'cidnum' => '', 'dmodem_fax_form' => '1', 'dmodem_fax_detect' => 'yes',
		'dmodem_fax_listen' => 'both', 'dmodem_fax_wait' => '6', 'dmodem_fax_ring' => 'yes',
		'gotodmodemfax' => 'dest', 'destdmodemfax' => 'app-dmodem,fax-sales,1']);
	$ext = new extensions();
	foreach ([['', 'Set', '__FROM_DID=${EXTEN}'], ['did-cid-hook', 'NoOp', 'CallerID Entry Point'],
		  ['dest-ext', 'Goto', 'app-dmodem,bbs,1']] as $st)
		$ext->add('ext-did-0001', '5551002', $st[0], new ext_dmodem_app($st[1], $st[2]));
	$m->doDialplanHook($ext, 'asterisk', 500);
	file_put_contents($dir . '/extensions_dmodem.conf', $ext->generateConf());
	@mkdir($dir . '/fax');
	/* fwconsole as the mail handler runs it: dmodem fax2mail through
	   fax2mail.php, the mail kept in <dir> */
	@mkdir($dir . '/bin');
	file_put_contents($dir . '/bin/fwconsole', "#!/bin/sh\nshift\nexec php " .
		escapeshellarg(__DIR__ . '/fax2mail.php') . ' ' . escapeshellarg($dir) . ' ' .
		escapeshellarg($engine) . ' ' . escapeshellarg(isset($argv[3]) ? $argv[3] : '18088') .
		' "$@"' . "\n");
	chmod($dir . '/bin/fwconsole', 0755);
	echo "gen.php: dmodem.conf and extensions_dmodem.conf ($engine) in $dir\n";
}
