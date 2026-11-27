<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// bin/mail2fax-spool.php <spool> - email to fax: the mail system gives a
// message on standard input (an alias with a pipe, for example
//   fax: "|/usr/bin/php /var/www/html/admin/modules/dmodem/bin/mail2fax-spool.php /var/spool/asterisk/fax/mail2fax"
// in /etc/aliases), and this script puts it into the spool folder as
// one .eml file.  The fax queue (fwconsole dmodem faxqueue, each minute)
// reads the spool.  The script runs as the user of the mail system, so
// it uses no code of FreePBX or of the module.  Exit codes: 0 the
// message is in the spool (or too big: dropped), 64 usage, 75 a
// temporary failure (the mail system tries again).

/* the most bytes of a message: as Mail2Fax::MAX_BYTES */
const MAIL2FAX_MAX_BYTES = 31457280;

/* the message on in (a stream) into folder dir as <time>-<random>.eml:
   first a temporary name, then a rename, so the queue reads only whole
   files.  The exit code */
function mail2fax_spool($in, $dir)
{
	if (!is_dir($dir) || !is_writable($dir)) {
		fwrite(STDERR, "mail2fax-spool: cannot write to $dir\n");
		return 75;
	}
	$b = stream_get_contents($in, MAIL2FAX_MAX_BYTES + 1);
	if ($b === false) {
		fwrite(STDERR, "mail2fax-spool: cannot read the message\n");
		return 75;
	}
	if (strlen($b) > MAIL2FAX_MAX_BYTES) {
		fwrite(STDERR, "mail2fax-spool: the message is too big, dropped\n");
		return 0;
	}
	$name = sprintf('%d-%s', time(), bin2hex(random_bytes(6)));
	$tmp = "$dir/.tmp-$name";
	if (file_put_contents($tmp, $b) !== strlen($b)) {
		if (is_file($tmp))
			unlink($tmp);
		fwrite(STDERR, "mail2fax-spool: cannot write $tmp\n");
		return 75;
	}
	chmod($tmp, 0644);
	if (!rename($tmp, "$dir/$name.eml")) {
		unlink($tmp);
		return 75;
	}
	return 0;
}

if (PHP_SAPI !== 'cli')
	exit(0);
if (count($argv) !== 2) {
	fwrite(STDERR, "Usage: mail2fax-spool.php <spool folder>\n");
	exit(64);
}
exit(mail2fax_spool(STDIN, rtrim($argv[1], '/')));
