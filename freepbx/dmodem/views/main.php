<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Neo-Desktop
//
// views/main.php - the page: its tabs, the last action's message, and the
// view's body.  Given $view, $message, $settings, $body.

if (!defined('FREEPBX_IS_AUTH')) {
	die('No direct script access allowed');
}
use FreePBX\modules\Dmodem as D;

$tabs = [
	'' => _('Profiles'),
	'settings' => _('Settings'),
	'dialout' => _('Dial out'),
	'check' => _('Line check'),
	'dialplan' => _('Generated files'),
];
$active = $view === 'form' ? '' : $view;
?>
<div class="container-fluid" id="dmodem">
	<h1><?php echo _('D-Modem'); ?></h1>
	<p class="text-muted">
		<?php echo _('Software data modems and fax on your calls: V.21 to V.34, and V.90 to a dial-up modem when the call is G.711 end to end.'); ?>
		<?php if ($settings['engine'] === 'ari'): ?>
			<?php echo sprintf(_('Calls go to dmodem-ari (Stasis app %s).'), D::h($settings['ari_app'])); ?>
		<?php else: ?>
			<?php echo _('Calls go to DModem() (app_dmodem).'); ?>
		<?php endif; ?>
	</p>
	<ul class="nav nav-tabs" role="tablist">
		<?php foreach ($tabs as $k => $label): ?>
			<li role="presentation" class="nav-item<?php echo $k === $active ? ' active' : ''; ?>">
				<a class="nav-link<?php echo $k === $active ? ' active' : ''; ?>"
				   href="?display=dmodem<?php echo $k !== '' ? '&amp;view=' . D::h($k) : ''; ?>">
					<?php echo D::h($label); ?></a>
			</li>
		<?php endforeach; ?>
	</ul>
	<?php if ($message): ?>
		<div class="alert alert-<?php echo D::h($message[0]); ?>" style="margin-top: 1em">
			<?php echo D::h($message[1]); ?>
		</div>
	<?php endif; ?>
	<div class="dmodem-body" style="margin-top: 1em">
		<?php echo $body; ?>
	</div>
</div>
