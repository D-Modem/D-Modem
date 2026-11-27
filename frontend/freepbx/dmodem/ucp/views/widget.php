<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// ucp/views/widget.php - the Fax widget of UCP: the folders Inbox, Sent
// and Failed (each fax: view as PDF, download the TIFF file, forward,
// delete), Outgoing (the faxes that wait: cancel), and Send.  Given
// $folders (inbox, sent, failed: uniqueid => entry), $jobs, $send (the
// user can send) and $job (the send form's defaults).  ucp/assets/js/
// global.js does the buttons.

/* s for HTML */
$h = function ($s) {
	return htmlspecialchars((string)$s, ENT_QUOTES, 'UTF-8');
};
$tabs = [
	'inbox' => _('Inbox'),
	'sent' => _('Sent'),
	'failed' => _('Failed'),
	'outgoing' => _('Outgoing'),
];
if ($send)
	$tabs['send'] = _('Send');
?>
<div class="dmodem-fax">
	<ul class="nav nav-tabs" role="tablist">
		<?php $first = true; foreach ($tabs as $k => $label): ?>
			<li class="nav-item"><a class="nav-link<?php echo $first ? ' active' : ''; ?>" data-toggle="tab"
				href="#" data-dmodem-tab="<?php echo $h($k); ?>" role="tab"><?php echo $h($label); ?>
				<?php if ($k !== 'send' && $k !== 'outgoing' && $folders[$k]): ?>
					<span class="badge badge-secondary"><?php echo count($folders[$k]); ?></span>
				<?php elseif ($k === 'outgoing' && $jobs): ?>
					<span class="badge badge-secondary"><?php echo count($jobs); ?></span>
				<?php endif; ?></a></li>
		<?php $first = false; endforeach; ?>
	</ul>
	<div class="dmodem-fax-panes">
		<?php $first = true; foreach (['inbox', 'sent', 'failed'] as $k): ?>
			<div class="dmodem-fax-pane" data-dmodem-pane="<?php echo $h($k); ?>"<?php echo $first ? '' : ' style="display: none"'; ?>>
				<?php if (!$folders[$k]): ?>
					<p class="text-muted"><?php echo _('No faxes.'); ?></p>
				<?php else: ?>
					<table class="table table-sm">
						<thead><tr>
							<th><?php echo _('Date'); ?></th>
							<th><?php echo $k === 'inbox' ? _('From') : _('To'); ?></th>
							<th><?php echo _('Pages'); ?></th>
							<th><?php echo _('Status'); ?></th>
							<th></th>
						</tr></thead>
						<tbody>
						<?php foreach ($folders[$k] as $u => $e): ?>
							<tr data-uniqueid="<?php echo $h($u); ?>">
								<td><?php echo $h(date('Y-m-d H:i', $e['time'])); ?></td>
								<td><?php echo $h($k === 'inbox' ? trim($e['cidname'] . ' ' . $e['cid']) : $e['number']); ?></td>
								<td><?php echo (int)$e['pages']; ?></td>
								<td><?php echo $h($e['status']); ?></td>
								<td class="text-nowrap">
									<?php if ($e['file'] !== ''): ?>
										<button type="button" class="btn btn-sm btn-link" data-dmodem-action="faxpdf" title="<?php echo _('View'); ?>"><i class="fa fa-file-pdf-o"></i></button>
										<button type="button" class="btn btn-sm btn-link" data-dmodem-action="faxtiff" title="<?php echo _('Download'); ?>"><i class="fa fa-download"></i></button>
										<?php if ($send): ?>
											<button type="button" class="btn btn-sm btn-link" data-dmodem-action="faxforward" title="<?php echo _('Forward'); ?>"><i class="fa fa-share"></i></button>
										<?php endif; ?>
									<?php endif; ?>
									<button type="button" class="btn btn-sm btn-link text-danger" data-dmodem-action="faxdelete" title="<?php echo _('Delete'); ?>"><i class="fa fa-trash"></i></button>
								</td>
							</tr>
						<?php endforeach; ?>
						</tbody>
					</table>
				<?php endif; ?>
			</div>
		<?php $first = false; endforeach; ?>
		<div class="dmodem-fax-pane" data-dmodem-pane="outgoing" style="display: none">
			<?php if (!$jobs): ?>
				<p class="text-muted"><?php echo _('No faxes wait.'); ?></p>
			<?php else: ?>
				<table class="table table-sm">
					<thead><tr>
						<th><?php echo _('Queued'); ?></th>
						<th><?php echo _('To'); ?></th>
						<th><?php echo _('Pages'); ?></th>
						<th><?php echo _('Tries'); ?></th>
						<th><?php echo _('Last result'); ?></th>
						<th></th>
					</tr></thead>
					<tbody>
					<?php foreach ($jobs as $id => $j): ?>
						<tr data-job="<?php echo $h($id); ?>">
							<td><?php echo $h(date('Y-m-d H:i', $j['created'])); ?></td>
							<td><?php echo $h($j['number']); ?></td>
							<td><?php echo (int)$j['pages']; ?></td>
							<td><?php echo (int)$j['attempts'] . ' / ' . ((int)$j['retries'] + 1); ?></td>
							<td><?php echo $h($j['state'] === 'sending' ? _('On a call') : $j['last']); ?></td>
							<td><?php if ($send && $j['state'] !== 'sending'): ?>
								<button type="button" class="btn btn-sm btn-link" data-dmodem-action="faxcancel"><?php echo _('Cancel'); ?></button>
							<?php endif; ?></td>
						</tr>
					<?php endforeach; ?>
					</tbody>
				</table>
			<?php endif; ?>
		</div>
		<?php if ($send): ?>
			<div class="dmodem-fax-pane" data-dmodem-pane="send" style="display: none">
				<form class="dmodem-fax-send" enctype="multipart/form-data">
					<div class="form-group"><label><?php echo _('Fax number'); ?></label>
						<input type="text" class="form-control" name="number" required></div>
					<div class="form-group"><label><?php echo _('Documents (PDF or TIFF)'); ?></label>
						<input type="file" class="form-control-file" name="docs[]" multiple accept=".pdf,.tif,.tiff,application/pdf,image/tiff"></div>
					<div class="form-check"><input type="checkbox" class="form-check-input" name="cover" value="yes" id="dmodem-cover">
						<label class="form-check-label" for="dmodem-cover"><?php echo _('Cover page'); ?></label></div>
					<div class="form-group"><label><?php echo _('To'); ?></label>
						<input type="text" class="form-control" name="to"></div>
					<div class="form-group"><label><?php echo _('From'); ?></label>
						<input type="text" class="form-control" name="from"></div>
					<div class="form-group"><label><?php echo _('Message'); ?></label>
						<textarea class="form-control" rows="3" name="message"></textarea></div>
					<div class="form-group"><label><?php echo _('Email the result'); ?></label>
						<select class="form-control" name="notify">
							<?php foreach (['never' => _('Never'), 'always' => _('Always'), 'success' => _('When it is sent'), 'failure' => _('When it is not sent')] as $v => $label): ?>
								<option value="<?php echo $h($v); ?>"<?php echo $v === $job['notify'] ? ' selected' : ''; ?>><?php echo $h($label); ?></option>
							<?php endforeach; ?>
						</select></div>
					<div class="form-group"><label><?php echo _('Email address'); ?></label>
						<input type="text" class="form-control" name="email" value="<?php echo $h($job['email']); ?>"></div>
					<input type="hidden" name="retries" value="<?php echo (int)$job['retries']; ?>">
					<button type="submit" class="btn btn-primary"><i class="fa fa-fax"></i> <?php echo _('Send'); ?></button>
				</form>
			</div>
		<?php endif; ?>
	</div>
</div>
