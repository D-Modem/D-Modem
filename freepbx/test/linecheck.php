<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Neo-Desktop
//
// linecheck.php <asterisk command> <endpoint> <verdict> [app|ari]: the
// module's line check (Dmodem::check()) on a running Asterisk, its CLI
// through "<asterisk command> -rx" in place of the manager; exit status
// 0 if the engine is up and endpoint's verdict is <verdict> (ok, info or
// warn).  For e2e.sh.

require_once __DIR__ . '/fpbx_env.php';

/* the manager's Command through the CLI */
class CliAstman extends FakeAstman
{
	public $ast;

	/* cmd's output from the running Asterisk */
	public function Command($cmd)
	{
		return ['data' => shell_exec($this->ast . ' -rx ' . escapeshellarg($cmd))];
	}
}

list(, $ast, $endpoint, $want) = $argv;
$engine = isset($argv[4]) ? $argv[4] : 'app';
$m = dmodem_new();
$m->FreePBX->astman = new CliAstman();
$m->FreePBX->astman->ast = $ast;
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
