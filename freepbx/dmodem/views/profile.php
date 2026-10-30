<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// views/profile.php - a profile's form.  Given $profile, $editing (the
// profile exists: its name stays) and $errors.

if (!defined('FREEPBX_IS_AUTH')) {
	die('No direct script access allowed');
}
use FreePBX\modules\Dmodem as D;

$p = $profile;
/* a form row: label, the field's HTML, its help; the field's error */
$row = function ($id, $label, $field, $help) use ($errors) {
	$err = isset($errors[$id]) ? $errors[$id] : '';
	?>
	<div class="element-container<?php echo $err !== '' ? ' has-error' : ''; ?>" data-field="<?php echo D::h($id); ?>">
		<div class="row form-group">
			<div class="col-md-3">
				<label class="control-label" for="<?php echo D::h($id); ?>"><?php echo D::h($label); ?></label>
			</div>
			<div class="col-md-9">
				<?php echo $field; ?>
				<?php if ($err !== ''): ?>
					<span class="help-block dmodem-error"><?php echo D::h($err); ?></span>
				<?php endif; ?>
				<span class="help-block"><?php echo D::h($help); ?></span>
			</div>
		</div>
	</div>
	<?php
};
/* a text input */
$text = function ($id, $value, $extra = '') {
	return '<input type="text" class="form-control" id="' . D::h($id) .
		'" name="' . D::h($id) . '" value="' . D::h($value) . '"' .
		$extra . '>';
};
/* a select of options value => label */
$select = function ($id, $value, array $options) {
	$h = '<select class="form-control" id="' . D::h($id) . '" name="' . D::h($id) . '">';
	foreach ($options as $v => $label)
		$h .= '<option value="' . D::h($v) . '"' .
			((string)$v === (string)$value ? ' selected' : '') . '>' .
			D::h($label) . '</option>';
	return $h . '</select>';
};
/* a yes/no checkbox */
$check = function ($id, $on) {
	return '<input type="checkbox" id="' . D::h($id) . '" name="' . D::h($id) .
		'" value="yes"' . ($on ? ' checked' : '') . '>';
};
$delete = $editing ? 'config.php?display=dmodem&action=delete&profile=' . urlencode($p['name']) : '';
?>
<form class="fpbx-submit" name="dmodem_profile" method="post"
      action="config.php?display=dmodem&amp;view=form<?php echo $editing ? '&amp;profile=' . urlencode($p['name']) : ''; ?>"
      <?php echo $delete !== '' ? 'data-fpbx-delete="' . D::h($delete) . '"' : ''; ?>>
	<input type="hidden" name="action" value="save_profile">
	<input type="hidden" name="original" value="<?php echo $editing ? D::h($p['name']) : ''; ?>">
	<h2><?php echo $editing ? sprintf(_('Profile %s'), D::h($p['name'])) : _('New profile'); ?></h2>

	<?php
	$row('name', _('Name'),
	     $text('name', $p['name'], $editing ? ' readonly' : ''),
	     $editing ? sprintf(_('The profile\'s destination is %s; its name stays.'), D::destination($p['name']))
		      : _('Letters, digits, - and _. The destination will be app-dmodem,<name>,1.'));
	$row('description', _('Description'), $text('description', $p['description']),
	     _('Shown with the destination.'));
	$row('mode', _('Data goes to'),
	     $select('mode', $p['mode'], [
		     'tcp' => _('A TCP server (a BBS, a telnet-less server)'),
		     'unix' => _('A server on a unix socket'),
		     'exec' => _('A program on a pty (pppd, a login, a door)'),
	     ]),
	     _('Where the call\'s data goes once the modems connect. Nothing reaches it before.'));
	$row('host', _('Host'), $text('host', $p['host']), _('The TCP server\'s address.'));
	$row('port', _('Port'), $text('port', $p['port']), _('The TCP server\'s port.'));
	$row('path', _('Socket'), $text('path', $p['path']), _('The unix socket\'s path.'));
	$row('command', _('Command'), $text('command', $p['command']),
	     _('Run by /bin/sh -c with the line as its terminal; DMODEM_CALLERID, DMODEM_RATE and the like in its environment. E.g. exec /usr/sbin/pppd nodetach noauth local lock passive 192.168.77.1:192.168.77.2'));
	$row('header', _('Call details first'), $check('header', $p['header']),
	     _('The server gets the call\'s details first: NAME=value lines and an empty line.'));
	$row('law', _('G.711 law'),
	     $select('law', $p['law'], [
		     'auto' => _('The channel\'s (auto)'),
		     'ulaw' => _('ulaw (North America, Japan)'),
		     'alaw' => _('alaw (Europe and most others)'),
	     ]),
	     _('The network\'s law. auto takes the channel\'s; set it when your trunk is fixed to one.'));
	$row('clear', _('ISDN data calls'),
	     $select('clear', $p['clear'], [
		     'auto' => _('Clear channel for ISDN data calls (auto)'),
		     'yes' => _('Every call is a clear channel'),
		     'no' => _('Never: G.711 audio only'),
	     ]),
	     _('An ISDN data call (a digital bearer from a BRI or PRI card, or a SIP call in CLEARMODE with codec_clearmode loaded) runs on a clear 64 kbit/s channel, X.75 by default; AT+B2=5 in the modem commands takes PPP instead, AT+B2=7 the modem\'s own error correction. A CLEARMODE call is clear whatever this says.'));
	$row('init', _('Modem commands'), $text('init', $p['init'], ' placeholder="AT..."'),
	     _('Extra AT commands for the modem, e.g. AT+MS=34 to stop at V.34, AT+MS=132 for V.32bis at most.'));
	$row('answer', _('Answer command'), $text('answer', $p['answer'], ' placeholder="ATA"'),
	     _('The command line the modem answers with (ATA when empty); commands for the answering call alone go before the A, e.g. AT+MS=132;A.'));
	$row('timeout', _('Connect timeout'), $text('timeout', $p['timeout']),
	     _('Seconds for the modems to connect.'));
	$row('fax', _('Take fax calls'), $check('fax', $p['fax']),
	     _('A fax machine calling (its CNG tone, or a V.8 fax call) is received as a fax, into the fax directory of the settings, instead of by the modem.'));
	?>
</form>
