<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// views/dialout.php - a modem call out (Dmodem::dialOut()).  Given
// $profiles, $call (the last one's profile, number, callerid) and $errors.

if (!defined('FREEPBX_IS_AUTH')) {
	die('No direct script access allowed');
}
use FreePBX\modules\Dmodem as D;

/* field id's error, as a help block */
$err = function ($id) use ($errors) {
	return isset($errors[$id]) ?
		'<span class="help-block dmodem-error">' . D::h($errors[$id]) . '</span>' : '';
};
?>
<p>
	<?php echo _('Calls a remote modem (a BBS, an ISP) through your outbound routes. Once it answers, D-Modem is the calling modem and the profile\'s server or program talks to the remote end: a terminal program on the profile\'s TCP port, or pppd for dial-up IP.'); ?>
</p>
<?php if (!$profiles): ?>
	<div class="alert alert-info"><?php echo _('Add a profile first.'); ?></div>
<?php else: ?>
	<form name="dmodem_dialout" method="post" action="config.php?display=dmodem&amp;view=dialout">
		<input type="hidden" name="action" value="dialout">
		<div class="row form-group<?php echo isset($errors['profile']) ? ' has-error' : ''; ?>">
			<div class="col-md-3"><label class="control-label" for="profile"><?php echo _('Profile'); ?></label></div>
			<div class="col-md-9">
				<select class="form-control" id="profile" name="profile">
					<?php foreach ($profiles as $name => $p): ?>
						<option value="<?php echo D::h($name); ?>"<?php echo $name === $call['profile'] ? ' selected' : ''; ?>>
							<?php echo D::h($name . ($p['description'] !== '' ? ' (' . $p['description'] . ')' : '')); ?></option>
					<?php endforeach; ?>
				</select>
				<?php echo $err('profile'); ?>
			</div>
		</div>
		<div class="row form-group<?php echo isset($errors['number']) ? ' has-error' : ''; ?>">
			<div class="col-md-3"><label class="control-label" for="number"><?php echo _('Number'); ?></label></div>
			<div class="col-md-9">
				<input type="text" class="form-control" id="number" name="number" value="<?php echo D::h($call['number']); ?>">
				<?php echo $err('number'); ?>
				<span class="help-block"><?php echo _('Dialled as an extension dials it: your outbound routes pick the trunk.'); ?></span>
			</div>
		</div>
		<div class="row form-group<?php echo isset($errors['callerid']) ? ' has-error' : ''; ?>">
			<div class="col-md-3"><label class="control-label" for="callerid"><?php echo _('Caller ID'); ?></label></div>
			<div class="col-md-9">
				<input type="text" class="form-control" id="callerid" name="callerid" value="<?php echo D::h($call['callerid']); ?>" placeholder="<?php echo _('the settings\''); ?>">
				<?php echo $err('callerid'); ?>
			</div>
		</div>
		<div class="row form-group">
			<div class="col-md-9 col-md-offset-3">
				<button type="submit" class="btn btn-primary" id="dmodem-dial"><i class="fa fa-phone"></i> <?php echo _('Dial'); ?></button>
			</div>
		</div>
	</form>
	<p class="text-muted">
		<?php echo _('From a script: fwconsole dmodem dial <profile> <number>. The call\'s result is in the Asterisk log and the CDR (DMODEMSTATUS, DMODEMRATE).'); ?>
	</p>
<?php endif; ?>
