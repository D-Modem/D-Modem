<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// linecheck.php <ami port> <endpoint> <verdict> [app|ari]: the module's
// line check (Dmodem::check()) on a running Asterisk, through its manager
// (127.0.0.1:<ami port>, user dmodem, secret dmodem); exit status 0 if
// the engine is up and endpoint's verdict is <verdict> (ok, info or
// warn).  For e2e.sh.

require_once __DIR__ . '/fpbx_env.php';

require_once __DIR__ . '/ami.php';

list(, $port, $endpoint, $want) = $argv;
$engine = isset($argv[4]) ? $argv[4] : 'app';
$m = dmodem_new();
$m->FreePBX->astman = new TestAmi($port, 'dmodem', 'dmodem');
$s = $m->getSettings();
$s['engine'] = $engine;
$c = $m->check($s);
$e = isset($c['endpoints'][$endpoint]) ? $c['endpoints'][$endpoint] : null;
echo "linecheck: engine $engine " . ($c['engine_ok'] ? 'up' : 'DOWN') . "\n";
foreach ($c['endpoints'] as $n => $x) {
	echo "linecheck: $n " . $x['worst'] . "\n";
	foreach ($x['findings'] as $f)
		echo '    ' . $f['level'] . ': ' . $f['text'] . "\n";
}
exit($c['engine_ok'] && $e && $e['worst'] === $want ? 0 : 1);
