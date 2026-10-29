<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// views/check.php - the line check: the engine's state and each PJSIP
// endpoint's settings for modem calls.  Given $check (Dmodem::check()).

if (!defined('FREEPBX_IS_AUTH')) {
	die('No direct script access allowed');
}
use FreePBX\modules\Dmodem as D;

$label = ['ok' => 'success', 'info' => 'info', 'warn' => 'warning'];
$word = ['ok' => _('OK'), 'info' => _('Note'), 'warn' => _('Fix')];
?>
<p>
	<?php echo _('V.90 and V.92 need G.711 of one law, untouched, end to end: no other codecs, no conversion between ulaw and alaw, nothing that listens to or changes the audio. This checks the PJSIP endpoints (your trunks and extensions) as Asterisk has them now. Your provider must do the same on their side: ask for G.711 with no transcoding.'); ?>
</p>
<?php if (!$check['manager']): ?>
	<div class="alert alert-danger"><?php echo _('Asterisk\'s manager interface is not connected: nothing to check.'); ?></div>
<?php else: ?>
	<?php if ($check['engine'] === 'ari'): ?>
		<div class="alert alert-<?php echo $check['engine_ok'] ? 'success' : 'danger'; ?>">
			<?php echo $check['engine_ok'] ? _('dmodem-ari is connected (its Stasis app is registered).')
				: _('dmodem-ari is not connected: its Stasis app is not registered. Start it (dmodem-ari -c /etc/asterisk/dmodem.conf), with the ARI user of the settings.'); ?>
		</div>
	<?php else: ?>
		<div class="alert alert-<?php echo $check['engine_ok'] ? 'success' : 'danger'; ?>">
			<?php echo $check['engine_ok'] ? _('app_dmodem is loaded.')
				: _('app_dmodem is not loaded: build it (asterisk/ in D-Modem), copy app_dmodem.so to Asterisk\'s modules and run module load app_dmodem.so.'); ?>
		</div>
	<?php endif; ?>
	<?php if (!$check['endpoints']): ?>
		<div class="alert alert-info"><?php echo _('No PJSIP endpoints.'); ?></div>
	<?php else: ?>
		<table class="table" id="dmodem-check">
			<thead>
				<tr>
					<th><?php echo _('Endpoint'); ?></th>
					<th><?php echo _('Codecs'); ?></th>
					<th><?php echo _('DTMF'); ?></th>
					<th><?php echo _('Findings'); ?></th>
				</tr>
			</thead>
			<tbody>
				<?php foreach ($check['endpoints'] as $name => $e): ?>
					<tr class="<?php echo $e['worst'] === 'warn' ? 'warning' : ''; ?>">
						<td><?php echo D::h($name); ?></td>
						<td><code><?php echo D::h(isset($e['settings']['allow']) ? implode(', ', $e['settings']['allow']) : '?'); ?></code></td>
						<td><?php echo D::h(isset($e['settings']['dtmf_mode']) ? $e['settings']['dtmf_mode'] : '?'); ?></td>
						<td>
							<?php foreach ($e['findings'] as $f): ?>
								<div><span class="label label-<?php echo $label[$f['level']]; ?>"><?php echo D::h($word[$f['level']]); ?></span>
									<?php echo D::h($f['text']); ?></div>
							<?php endforeach; ?>
						</td>
					</tr>
				<?php endforeach; ?>
			</tbody>
		</table>
	<?php endif; ?>
	<p class="text-muted">
		<?php echo _('Also off on the modem\'s inbound routes: Fax Detect (Inbound Routes > Fax), call recording, music on hold and announcements before the answer.'); ?>
	</p>
<?php endif; ?>
