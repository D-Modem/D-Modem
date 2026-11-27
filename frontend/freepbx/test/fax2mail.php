<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// fax2mail.php <dir> <engine> <ari port> <subcommand> <args>: fwconsole
// dmodem fax2mail (or faxsent) as the hangup handlers of e2e.sh's
// Asterisk run it (gen.php
// writes <dir>/bin/fwconsole), on the module that gen.php sets up.  The
// mail system is a stand-in: each message goes to <dir>/mail-<n>.eml
// (To:, Subject:, the headers, the body), and the command's lines and its
// arguments to <dir>/fax2mail.log.

require_once __DIR__ . '/gen.php';

use FreePBX\modules\Dmodem\Cli;

$dir = $argv[1];
$m = dmodem_test_module($dir, $argv[2], $argv[3]);
dmodem_test_store($dir, true);
$args = array_slice($argv, 4);
$log = fopen($dir . '/fax2mail.log', 'a');
fwrite($log, 'args: ' . json_encode($args) . "\n");
$n = 0;
/* the stand-in mail system: the message into a file */
$send = function ($to, $subject, $body, $headers) use ($dir, &$n) {
	$n++;
	$f = sprintf('%s/mail-%d-%d.eml', $dir, getmypid(), $n);
	return file_put_contents($f, "To: $to\r\nSubject: $subject\r\n$headers\r\n\r\n$body") !== false;
};
$st = Cli::run($m, $args, ['send' => $send], function ($line) use ($log) {
	fwrite($log, $line . "\n");
	echo $line . "\n";
});
fwrite($log, "status: $st\n");
dmodem_test_store($dir, false);
exit($st);
