<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// views/didfax.php - the D-Modem fax tab of an inbound route (Core's
// page, display=did): the route's fax detection.  Given $f (its
// settings, Config::defaultDidFax()) and $settings.

if (!defined('FREEPBX_IS_AUTH')) {
	die('No direct script access allowed');
}
use FreePBX\modules\Dmodem as D;

/* a form row: label, the field's HTML, its help */
$row = function ($id, $label, $field, $help) {
	?>
	<div class="element-container" data-field="<?php echo D::h($id); ?>">
		<div class="row form-group">
			<div class="col-md-3">
				<label class="control-label" for="<?php echo D::h($id); ?>"><?php echo D::h($label); ?></label>
			</div>
			<div class="col-md-9">
				<?php echo $field; ?>
				<span class="help-block"><?php echo D::h($help); ?></span>
			</div>
		</div>
	</div>
	<?php
};
$listen = '<select class="form-control" id="dmodem_fax_listen" name="dmodem_fax_listen">';
foreach (['both' => _('The calling tone (CNG) and T.38'), 'cng' => _('The calling tone (CNG) only'),
	  't38' => _('T.38 only')] as $v => $label)
	$listen .= '<option value="' . $v . '"' . ($f['listen'] === $v ? ' selected' : '') . '>' .
		D::h($label) . '</option>';
$listen .= '</select>';
/* the destination picker of FreePBX (a text field without it) */
$dest = function_exists('drawselects') ?
	drawselects($f['dest'], 'dmodemfax', false, false) :
	'<input type="text" class="form-control" id="dmodem_fax_dest" name="dmodem_fax_dest" value="' .
	D::h($f['dest']) . '">';
?>
<div id="dmodem-didfax">
	<input type="hidden" name="dmodem_fax_form" value="1">
	<p class="text-muted">
		<?php echo _('D-Modem answers the call and listens for a fax machine for the time below. A fax goes to the fax destination. Other calls go on to the route\'s destination. Use this or the Fax tab of the Fax module on a route, not both.'); ?>
		<?php echo $settings['engine'] === 'ari' ? _('dmodem-ari hears a T.38 request only with its SIP side on (the settings).') : ''; ?>
	</p>
	<?php
	$row('dmodem_fax_detect', _('Detect faxes'),
	     '<input type="checkbox" id="dmodem_fax_detect" name="dmodem_fax_detect" value="yes"' .
	     ($f['detect'] ? ' checked' : '') . '>',
	     _('Listen for a fax machine on the calls of this route.'));
	$row('dmodem_fax_listen', _('Listen for'), $listen,
	     _('A fax machine sends its calling tone (CNG) or asks for T.38.'));
	$row('dmodem_fax_wait', _('Time'),
	     '<input type="number" class="form-control" id="dmodem_fax_wait" name="dmodem_fax_wait" min="1" max="30" value="' .
	     D::h($f['wait']) . '">',
	     _('How long D-Modem listens, in seconds (1 to 30). Most fax machines send the calling tone within 4 seconds.'));
	$row('dmodem_fax_ring', _('Ring while listening'),
	     '<input type="checkbox" id="dmodem_fax_ring" name="dmodem_fax_ring" value="yes"' .
	     ($f['ring'] ? ' checked' : '') . '>',
	     _('A person who calls hears the ring tone, not silence, while D-Modem listens.'));
	$row('dmodem_fax_dest', _('Fax destination'), $dest,
	     _('Where a fax goes: for example a D-Modem fax recipient (fax to email), or "D-Modem: receive a fax".'));
	?>
</div>
<script>
// the fields of the fax detection shown when it is on
$(function () {
	function show() {
		var on = $('#dmodem_fax_detect').is(':checked');
		$('#dmodem-didfax .element-container').not('[data-field="dmodem_fax_detect"]').toggle(on);
	}
	$('#dmodem_fax_detect').on('change', show);
	show();
});
</script>
