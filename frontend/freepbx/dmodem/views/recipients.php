<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// views/recipients.php - the fax recipients (fax to email), each a
// destination.  Given $recipients and $settings.

if (!defined('FREEPBX_IS_AUTH')) {
	die('No direct script access allowed');
}
use FreePBX\modules\Dmodem as D;
?>
<p>
	<a href="?display=dmodem&amp;view=recipient" class="btn btn-default">
		<i class="fa fa-plus"></i> <?php echo _('Add fax recipient'); ?></a>
</p>
<?php if (!$recipients): ?>
	<div class="alert alert-info">
		<?php echo _('No fax recipients yet. A fax recipient is a destination for your inbound routes, IVRs and time conditions, under "D-Modem": D-Modem takes the fax and, when the call ends, mails it to the recipient as a PDF file, a TIFF file or both.'); ?>
	</div>
<?php else: ?>
	<table class="table table-striped" id="dmodem-recipients">
		<thead>
			<tr>
				<th><?php echo _('Id'); ?></th>
				<th><?php echo _('Name'); ?></th>
				<th><?php echo _('Email'); ?></th>
				<th><?php echo _('Attach'); ?></th>
				<th><?php echo _('Destination'); ?></th>
			</tr>
		</thead>
		<tbody>
			<?php foreach ($recipients as $id => $r): ?>
				<tr>
					<td><a href="?display=dmodem&amp;view=recipient&amp;recipient=<?php echo urlencode($id); ?>">
						<?php echo D::h($id); ?></a></td>
					<td><?php echo D::h($r['name']); ?></td>
					<td><?php echo $r['email'] !== '' ? D::h($r['email']) :
						sprintf(_('User Manager: %s'), D::h($r['user'])); ?></td>
					<td><?php echo D::h($r['attach'] !== '' ? $r['attach'] :
						sprintf(_('default (%s)'), $settings['fax_attach'])); ?></td>
					<td><code><?php echo D::h(D::recipientDestination($id)); ?></code></td>
				</tr>
			<?php endforeach; ?>
		</tbody>
	</table>
<?php endif; ?>
