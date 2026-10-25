<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Neo-Desktop
//
// gen.php <dir> [app|ari] [ari port]: what the FreePBX module writes on
// Apply Config, made the way the page makes it (its forms posted through
// doConfigPageInit()): <dir>/dmodem.conf and <dir>/extensions_dmodem.conf
// (the app-dmodem context, as FreePBX writes extensions_additional.conf).
// The profiles: bbs (TCP 127.0.0.1:2323, sip_call.py's BBS) and the fax
// destination.  For e2e.sh.

require_once __DIR__ . '/fpbx_env.php';

$dir = isset($argv[1]) ? $argv[1] : '.';
$engine = isset($argv[2]) ? $argv[2] : 'app';
$port = isset($argv[3]) ? $argv[3] : '18088';

$m = dmodem_new();
dmodem_post($m, ['action' => 'save_settings', 'engine' => $engine, 'debug' => '1',
		 'fax_dir' => $dir . '/fax', 'fax_max_rate' => '14400',
		 'fax_ecm' => 'yes', 'fax_t38' => 'yes', 'fax_destination' => 'yes',
		 'ari_url' => 'http://127.0.0.1:' . $port, 'ari_user' => 'dmodem',
		 'ari_password' => 'dmodem', 'ari_app' => 'dmodem',
		 'sip_endpoint' => 'dmodem', 'sip_bind' => '127.0.0.1:5090']);
dmodem_post($m, ['action' => 'save_profile', 'original' => '', 'name' => 'bbs',
		 'description' => 'the test BBS', 'mode' => 'tcp', 'host' => '127.0.0.1',
		 'port' => '2323', 'law' => 'auto', 'timeout' => '60', 'fax' => 'yes']);
if ($m->errors) {
	fwrite(STDERR, 'gen.php: ' . json_encode($m->errors) . "\n");
	exit(1);
}
$m->writeConfig($m->genConfig());
file_put_contents($dir . '/dmodem.conf', $m->FreePBX->written['dmodem.conf']);
file_put_contents($dir . '/extensions_dmodem.conf', dmodem_dialplan($m));
@mkdir($dir . '/fax');
echo "gen.php: dmodem.conf and extensions_dmodem.conf ($engine) in $dir\n";
