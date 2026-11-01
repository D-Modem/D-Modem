<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// views/dialplan.php - what Apply Config writes: dmodem.conf and the
// app-dmodem context.  Given $conf and $dialplan.

if (!defined('FREEPBX_IS_AUTH')) {
	die('No direct script access allowed');
}
use FreePBX\modules\Dmodem as D;
?>
<p><?php echo _('What Apply Config writes from the saved profiles and settings.'); ?></p>
<h2>/etc/asterisk/dmodem.conf</h2>
<pre id="dmodem-conf"><?php echo D::h($conf); ?></pre>
<h2><?php echo _('Dialplan'); ?> (extensions_additional.conf)</h2>
<pre id="dmodem-dialplan"><?php echo D::h($dialplan); ?></pre>
