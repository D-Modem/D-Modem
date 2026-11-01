<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// views/profiles.php - the profiles, each a destination.  Given $profiles.

if (!defined('FREEPBX_IS_AUTH')) {
	die('No direct script access allowed');
}
use FreePBX\modules\Dmodem as D;

/* where profile p's data goes, in a few words */
$target = function (array $p) {
	switch ($p['mode']) {
	case 'tcp':
		return 'tcp ' . $p['host'] . ':' . $p['port'];
	case 'unix':
		return 'unix ' . $p['path'];
	default:
		return 'exec ' . $p['command'];
	}
};
?>
<p>
	<a href="?display=dmodem&amp;view=form" class="btn btn-default">
		<i class="fa fa-plus"></i> <?php echo _('Add profile'); ?></a>
</p>
<?php if (!$profiles): ?>
	<div class="alert alert-info">
		<?php echo _('No profiles yet. A profile says where a call\'s data goes once the modems connect: a BBS or other server (TCP or a unix socket), or a program on a pty such as pppd. Each profile is a destination for your inbound routes, IVRs and time conditions, under "D-Modem".'); ?>
	</div>
<?php else: ?>
	<table class="table table-striped" id="dmodem-profiles">
		<thead>
			<tr>
				<th><?php echo _('Profile'); ?></th>
				<th><?php echo _('Description'); ?></th>
				<th><?php echo _('Data goes to'); ?></th>
				<th><?php echo _('Law'); ?></th>
				<th><?php echo _('Fax'); ?></th>
				<th><?php echo _('Destination'); ?></th>
			</tr>
		</thead>
		<tbody>
			<?php foreach ($profiles as $name => $p): ?>
				<tr>
					<td><a href="?display=dmodem&amp;view=form&amp;profile=<?php echo urlencode($name); ?>">
						<?php echo D::h($name); ?></a></td>
					<td><?php echo D::h($p['description']); ?></td>
					<td><code><?php echo D::h($target($p)); ?></code></td>
					<td><?php echo D::h($p['law']); ?></td>
					<td><?php echo $p['fax'] ? _('yes') : _('no'); ?></td>
					<td><code><?php echo D::h(D::destination($name)); ?></code></td>
				</tr>
			<?php endforeach; ?>
		</tbody>
	</table>
<?php endif; ?>
