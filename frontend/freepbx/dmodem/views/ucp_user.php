<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// views/ucp_user.php - the D-Modem Fax part of the UCP tab of a User
// Manager user or group (Dmodem::ucpConfigPage()).  Given $mode (user or
// group), $enable, $send (true, false, or null: not set), $chosen (the
// recipient ids given) and $recipients.

if (!defined('FREEPBX_IS_AUTH')) {
	die('No direct script access allowed');
}
use FreePBX\modules\Dmodem as D;

/* a yes / no (/ inherit for a user) radio set name of value v */
$radios = function ($name, $v) use ($mode) {
	$opts = ['yes' => _('Yes'), 'no' => _('No')];
	if ($mode === 'user')
		$opts['inherit'] = _('Inherit');
	$cur = $v === null ? ($mode === 'user' ? 'inherit' : 'no') : ($v ? 'yes' : 'no');
	$h = '<span class="radioset">';
	foreach ($opts as $k => $label) {
		$id = D::h($name . '_' . $k);
		$h .= '<input type="radio" name="' . D::h($name) . '" id="' . $id . '" value="' . $k . '"' .
			($k === $cur ? ' checked' : '') . '><label for="' . $id . '">' . D::h($label) . '</label>';
	}
	return $h . '</span>';
};
$rows = [
	['dmodem_fax_enable', _('Fax widget'), $radios('dmodem_fax_enable', $enable),
	 _('The Fax widget in UCP: the faxes received for the recipients below and the faxes that this user sent.')],
	['dmodem_fax_send', _('Send faxes'), $radios('dmodem_fax_send', $send),
	 _('The widget can send and forward faxes.')],
];
?>
<?php foreach ($rows as $r): ?>
	<div class="element-container">
		<div class="row form-group">
			<div class="col-md-3"><label class="control-label" for="<?php echo D::h($r[0]); ?>"><?php echo D::h($r[1]); ?></label></div>
			<div class="col-md-9"><?php echo $r[2]; ?>
				<span class="help-block"><?php echo D::h($r[3]); ?></span></div>
		</div>
	</div>
<?php endforeach; ?>
<div class="element-container">
	<div class="row form-group">
		<div class="col-md-3"><label class="control-label" for="dmodem_fax_recipients"><?php echo _('Fax recipients'); ?></label></div>
		<div class="col-md-9">
			<select class="form-control" id="dmodem_fax_recipients" name="dmodem_fax_recipients[]" multiple>
				<?php foreach ($recipients as $id => $r): ?>
					<option value="<?php echo D::h($id); ?>"<?php echo in_array($id, $chosen, true) ? ' selected' : ''; ?>>
						<?php echo D::h($r['name'] . ' (' . $id . ')'); ?></option>
				<?php endforeach; ?>
			</select>
			<span class="help-block"><?php echo _('The faxes of these recipients show in the widget. A recipient of this User Manager user always shows.'); ?></span>
		</div>
	</div>
</div>
