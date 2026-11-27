<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// views/send.php - a fax to send (Dmodem::queueFax()): the number, the
// documents (PDF or TIFF), the cover page, the tries and the email of the
// result; then the queue, with a cancel button for each fax that waits.
// Given $job (the form's values), $jobs (the queue), $settings, $errors.

if (!defined('FREEPBX_IS_AUTH')) {
	die('No direct script access allowed');
}
use FreePBX\modules\Dmodem as D;
use FreePBX\modules\Dmodem\FaxSend;

/* field id's error, as a help block */
$err = function ($id) use ($errors) {
	return isset($errors[$id]) ?
		'<span class="help-block dmodem-error">' . D::h($errors[$id]) . '</span>' : '';
};
/* the start of a form row for field id with label */
$row = function ($id, $label) use ($errors) {
	return '<div class="row form-group' . (isset($errors[$id]) ? ' has-error' : '') .
		'"><div class="col-md-3"><label class="control-label" for="' . D::h($id) . '">' .
		D::h($label) . '</label></div><div class="col-md-9">';
};
$notify = [
	'never' => _('Never'),
	'always' => _('Always'),
	'success' => _('When it is sent'),
	'failure' => _('When it is not sent'),
];
?>
<p>
	<?php echo _('Sends a fax through your outbound routes. D-Modem makes the fax pages from PDF and TIFF files with Ghostscript, puts the cover page first, and tries again when the call fails. A sent fax goes into the Faxes list.'); ?>
</p>
<form name="dmodem_sendfax" method="post" enctype="multipart/form-data"
      action="config.php?display=dmodem&amp;view=send">
	<input type="hidden" name="action" value="send_fax">
	<?php echo $row('number', _('Fax number')); ?>
		<input type="text" class="form-control" id="number" name="number" value="<?php echo D::h($job['number']); ?>">
		<?php echo $err('number'); ?>
		<span class="help-block"><?php echo _('Dialled as an extension dials it: your outbound routes pick the trunk.'); ?></span>
	</div></div>
	<?php echo $row('docs', _('Documents')); ?>
		<input type="file" id="docs" name="docs[]" multiple accept=".pdf,.tif,.tiff,application/pdf,image/tiff">
		<?php echo $err('docs'); ?>
		<span class="help-block"><?php echo sprintf(_('PDF or TIFF files, %d at most. They are sent in this order.'), FaxSend::MAX_DOCS); ?></span>
	</div></div>
	<?php echo $row('cover', _('Cover page')); ?>
		<input type="checkbox" id="cover" name="cover" value="yes"<?php echo $job['cover'] ? ' checked' : ''; ?>>
		<span class="help-block"><?php echo _('A first page with your company (Settings), the recipient, the number of pages and the message.'); ?></span>
	</div></div>
	<?php echo $row('to', _('To')); ?>
		<input type="text" class="form-control" id="to" name="to" value="<?php echo D::h($job['to']); ?>">
		<?php echo $err('to'); ?>
	</div></div>
	<?php echo $row('from', _('From')); ?>
		<input type="text" class="form-control" id="from" name="from" value="<?php echo D::h($job['from']); ?>">
		<?php echo $err('from'); ?>
	</div></div>
	<?php echo $row('message', _('Message')); ?>
		<textarea class="form-control" rows="5" id="message" name="message"><?php echo D::h($job['message']); ?></textarea>
		<?php echo $err('message'); ?>
	</div></div>
	<?php echo $row('retries', _('Tries again')); ?>
		<select class="form-control" id="retries" name="retries">
			<?php for ($i = 0; $i <= 3; $i++): ?>
				<option value="<?php echo $i; ?>"<?php echo $i === $job['retries'] ? ' selected' : ''; ?>><?php echo $i; ?></option>
			<?php endfor; ?>
		</select>
		<?php echo $err('retries'); ?>
		<span class="help-block"><?php echo sprintf(_('The tries after the first, %d minutes apart (Settings).'), (int)$settings['fax_retry_delay']); ?></span>
	</div></div>
	<?php echo $row('notify', _('Email the result')); ?>
		<select class="form-control" id="notify" name="notify">
			<?php foreach ($notify as $k => $label): ?>
				<option value="<?php echo D::h($k); ?>"<?php echo $k === $job['notify'] ? ' selected' : ''; ?>><?php echo D::h($label); ?></option>
			<?php endforeach; ?>
		</select>
		<?php echo $err('notify'); ?>
	</div></div>
	<?php echo $row('email', _('Email address')); ?>
		<input type="text" class="form-control" id="email" name="email" value="<?php echo D::h($job['email']); ?>">
		<?php echo $err('email'); ?>
	</div></div>
	<div class="row form-group">
		<div class="col-md-9 col-md-offset-3">
			<button type="submit" class="btn btn-primary" id="dmodem-sendfax"><i class="fa fa-fax"></i> <?php echo _('Send'); ?></button>
		</div>
	</div>
</form>

<h3><?php echo _('Waiting to be sent'); ?></h3>
<?php if (!$jobs): ?>
	<p class="text-muted"><?php echo _('No faxes wait.'); ?></p>
<?php else: ?>
	<table class="table table-striped" id="dmodem-faxjobs">
		<thead>
			<tr>
				<th><?php echo _('Queued'); ?></th>
				<th><?php echo _('Fax number'); ?></th>
				<th><?php echo _('To'); ?></th>
				<th><?php echo _('Pages'); ?></th>
				<th><?php echo _('State'); ?></th>
				<th><?php echo _('Tries'); ?></th>
				<th><?php echo _('Last result'); ?></th>
				<th></th>
			</tr>
		</thead>
		<tbody>
			<?php foreach ($jobs as $id => $j): ?>
				<tr>
					<td><?php echo D::h(date('Y-m-d H:i', $j['created'])); ?></td>
					<td><?php echo D::h($j['number']); ?></td>
					<td><?php echo D::h($j['to']); ?></td>
					<td><?php echo (int)$j['pages']; ?></td>
					<td><?php echo D::h($j['state'] === 'sending' ? _('On a call') :
						($j['state'] === 'retry' ? sprintf(_('Next try %s'), date('H:i', $j['next'])) :
						 _('Queued'))); ?></td>
					<td><?php echo (int)$j['attempts'] . ' / ' . ((int)$j['retries'] + 1); ?></td>
					<td><?php echo D::h($j['last']); ?></td>
					<td>
						<?php if ($j['state'] !== 'sending'): ?>
							<form method="post" action="?display=dmodem&amp;view=send" style="display: inline">
								<input type="hidden" name="action" value="cancel_fax">
								<input type="hidden" name="job" value="<?php echo D::h($id); ?>">
								<button type="submit" class="btn btn-default btn-xs"><?php echo _('Cancel'); ?></button>
							</form>
						<?php endif; ?>
					</td>
				</tr>
			<?php endforeach; ?>
		</tbody>
	</table>
<?php endif; ?>
