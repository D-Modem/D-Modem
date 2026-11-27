<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// views/faxes.php - the fax log: each received fax, newest first, with
// its PDF to view, its TIFF file to keep, and a delete button.  Given
// $faxes, $recipients and $settings.

if (!defined('FREEPBX_IS_AUTH')) {
	die('No direct script access allowed');
}
use FreePBX\modules\Dmodem as D;
use FreePBX\modules\Dmodem\FaxLog;

/* the Ajax link of command cmd for the fax of call u */
$link = function ($cmd, $u) {
	return 'ajax.php?module=dmodem&amp;command=' . $cmd . '&amp;uniqueid=' . urlencode($u);
};
?>
<p class="text-muted">
	<?php echo (int)$settings['fax_keep_days'] > 0 ?
		sprintf(_('Each received fax stays here, and in %s, for %d days (Settings).'),
			D::h($settings['fax_dir']), (int)$settings['fax_keep_days']) :
		sprintf(_('Each received fax stays here, and in %s, until you delete it. Settings can delete old faxes.'),
			D::h($settings['fax_dir'])); ?>
</p>
<?php if (!$faxes): ?>
	<div class="alert alert-info">
		<?php echo _('No faxes yet. A fax that D-Modem takes (the fax destination, a fax recipient, or a profile with fax) comes here when the call ends.'); ?>
	</div>
<?php else: ?>
	<table class="table table-striped" id="dmodem-faxes">
		<thead>
			<tr>
				<th><?php echo _('Date'); ?></th>
				<th><?php echo _('From'); ?></th>
				<th><?php echo _('Fax number'); ?></th>
				<th><?php echo _('To'); ?></th>
				<th><?php echo _('Pages'); ?></th>
				<th><?php echo _('Status'); ?></th>
				<th><?php echo _('Rate'); ?></th>
				<th><?php echo _('Email'); ?></th>
				<th></th>
			</tr>
		</thead>
		<tbody>
			<?php foreach ($faxes as $u => $e): ?>
				<?php $file = $dmodem->faxPath($e) !== null; ?>
				<tr>
					<td><?php echo D::h(date('Y-m-d H:i', $e['time'])); ?></td>
					<td><?php echo D::h(trim($e['cidname'] . ' ' . $e['cid'])); ?></td>
					<td><?php echo D::h($e['remote']); ?></td>
					<td><?php echo $e['recipient'] === '' ? '' :
						D::h(isset($recipients[$e['recipient']]) ?
						     $recipients[$e['recipient']]['name'] : $e['recipient']); ?></td>
					<td><?php echo (int)$e['pages']; ?></td>
					<td><?php echo D::h(FaxLog::status($e)); ?></td>
					<td><?php echo $e['rate'] > 0 ? (int)$e['rate'] : ''; ?></td>
					<td><?php if ($e['mail'] !== ''): ?>
						<span class="<?php echo $e['mail_ok'] ? 'text-success' : 'text-danger'; ?>"
						      title="<?php echo D::h($e['mail']); ?>">
							<?php echo $e['mail_ok'] ? _('Sent') : _('Not sent'); ?></span>
					<?php endif; ?></td>
					<td class="text-nowrap">
						<?php if ($file): ?>
							<a class="btn btn-default btn-xs" target="_blank" rel="noopener"
							   href="<?php echo $link('faxpdf', $u); ?>"><?php echo _('View PDF'); ?></a>
							<a class="btn btn-default btn-xs"
							   href="<?php echo $link('faxtiff', $u); ?>"><?php echo _('TIFF'); ?></a>
						<?php else: ?>
							<span class="text-muted"><?php echo _('No file'); ?></span>
						<?php endif; ?>
						<form method="post" action="?display=dmodem&amp;view=faxes" style="display: inline"
						      onsubmit="return confirm(<?php echo D::h(json_encode(_('Delete this fax and its file?'))); ?>);">
							<input type="hidden" name="action" value="delete_fax">
							<input type="hidden" name="uniqueid" value="<?php echo D::h($u); ?>">
							<button type="submit" class="btn btn-danger btn-xs"><?php echo _('Delete'); ?></button>
						</form>
					</td>
				</tr>
			<?php endforeach; ?>
		</tbody>
	</table>
<?php endif; ?>
