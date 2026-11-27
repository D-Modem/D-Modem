<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// sendfax.php <dir> <app|ari> <ami port> <subcommand> [args...]:
// fwconsole dmodem sendfax, faxjobs, faxes and the like (Cli::run()) on
// the module that gen.php sets up, against a running Asterisk (its
// manager on 127.0.0.1:<ami port>, user dmodem, secret dmodem).  The fax
// queue and the fax log stay in <dir>/faxstore.json between this script
// and fax2mail.php (the hangup handlers' fwconsole).  Options of sendfax:
// cover=yes, message=..., retries=n, notify=never (as key=value after
// the subcommand's arguments).

require_once __DIR__ . '/gen.php';
require_once __DIR__ . '/ami.php';

$dir = $argv[1];
$m = dmodem_test_module($dir, $argv[2], '18088');
$m->FreePBX->astman = new TestAmi($argv[3], 'dmodem', 'dmodem');
dmodem_test_store($dir, true);
$args = [];
$opts = [];
foreach (array_slice($argv, 4) as $a) {
	if (preg_match('/^([a-z]+)=(.*)$/s', $a, $kv))
		$opts[$kv[1]] = $kv[2];
	else
		$args[] = $a;
}
$st = \FreePBX\modules\Dmodem\Cli::run($m, $args, $opts, function ($l) {
	echo "sendfax.php: $l\n";
});
dmodem_test_store($dir, false);
exit($st);
