<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Neo-Desktop
//
// dial.php <ami port> <app|ari> <profile> <number>: the module's dial-out
// (fwconsole dmodem dial, Cli::run()) against a running Asterisk, its
// manager on 127.0.0.1:<ami port> (user dmodem, secret dmodem).  Exit
// status 0 if Asterisk took the call.  For e2e.sh, as sip_call.py's
// SIP_IN.

require_once __DIR__ . '/gen.php';
require_once __DIR__ . '/ami.php';

list(, $port, $engine, $profile, $number) = $argv;
$m = dmodem_test_module(sys_get_temp_dir(), $engine, '18088');
$m->FreePBX->astman = new TestAmi($port, 'dmodem', 'dmodem');
exit(\FreePBX\modules\Dmodem\Cli::run($m, ['dial', $profile, $number], [],
	function ($l) {
		echo "dial.php: $l\n";
	}));
