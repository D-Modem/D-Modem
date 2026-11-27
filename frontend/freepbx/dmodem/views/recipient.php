<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// views/recipient.php - a fax recipient's form.  Given $recipient,
// $editing (the recipient exists: its id stays), $users (User Manager's
// users, username => email; null if it is not installed) and $errors.

if (!defined('FREEPBX_IS_AUTH')) {
	die('No direct script access allowed');
}
use FreePBX\modules\Dmodem as D;

$r = $recipient;
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
$delete = $editing ? 'config.php?display=dmodem&action=delete_recipient&recipient=' . urlencode($r['id']) : '';
?>
<form class="fpbx-submit" name="dmodem_recipient" method="post"
      action="config.php?display=dmodem&amp;view=recipient<?php echo $editing ? '&amp;recipient=' . urlencode($r['id']) : ''; ?>"
      <?php echo $delete !== '' ? 'data-fpbx-delete="' . D::h($delete) . '"' : ''; ?>>
	<input type="hidden" name="action" value="save_recipient">
	<input type="hidden" name="original" value="<?php echo $editing ? D::h($r['id']) : ''; ?>">
	<h2><?php echo $editing ? sprintf(_('Fax recipient %s'), D::h($r['id'])) : _('New fax recipient'); ?></h2>

	<?php
	$row('id', _('Id'),
	     $text('id', $r['id'], $editing ? ' readonly' : ''),
	     $editing ? sprintf(_('The recipient\'s destination is %s; its id stays.'), D::recipientDestination($r['id']))
		      : _('Small letters, digits and _. The destination will be app-dmodem,fax-<id>,1.'));
	$row('name', _('Name'), $text('name', $r['name']),
	     _('Shown with the destination, and in the message as {recipient}.'));
	$row('email', _('Email address'), $text('email', $r['email']),
	     _('Where the faxes go. Empty: the User Manager user\'s address.'));
	if ($users !== null) {
		$opts = ['' => _('None')];
		foreach ($users as $u => $mail)
			$opts[$u] = $u . ' (' . $mail . ')';
		if ($r['user'] !== '' && !isset($opts[$r['user']]))
			$opts[$r['user']] = $r['user'];
		$row('user', _('User Manager user'), $select('user', $r['user'], $opts),
		     _('The faxes go to this user\'s address when the email address is empty. A change of the user\'s address applies at once.'));
	} else {
		echo '<input type="hidden" name="user" value="' . D::h($r['user']) . '">';
	}
	$row('attach', _('Attach'), $select('attach', $r['attach'], [
		     '' => _('The settings\' default'),
		     'pdf' => _('A PDF file'),
		     'tiff' => _('The TIFF file'),
		     'both' => _('Both'),
	     ]),
	     _('What goes with the message.'));
	?>
</form>
