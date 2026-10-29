<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// views/settings.php - the module's settings ([general] of dmodem.conf,
// the engine, the fax destination).  Given $s and $errors.

if (!defined('FREEPBX_IS_AUTH')) {
	die('No direct script access allowed');
}
use FreePBX\modules\Dmodem as D;
use FreePBX\modules\Dmodem\Config;

/* a form row: label, the field's HTML, its help; the field's error */
$row = function ($id, $label, $field, $help, $class = '') use ($errors) {
	$err = isset($errors[$id]) ? $errors[$id] : '';
	?>
	<div class="element-container <?php echo D::h($class); ?><?php echo $err !== '' ? ' has-error' : ''; ?>" data-field="<?php echo D::h($id); ?>">
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
$text = function ($id, $value, $type = 'text') {
	return '<input type="' . $type . '" class="form-control" id="' . D::h($id) .
		'" name="' . D::h($id) . '" value="' . D::h($value) . '">';
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
/* a yes/no checkbox */
$check = function ($id, $on) {
	return '<input type="checkbox" id="' . D::h($id) . '" name="' . D::h($id) .
		'" value="yes"' . ($on ? ' checked' : '') . '>';
};
$rates = [];
foreach (Config::FAX_RATES as $r)
	$rates[$r] = sprintf(_('%d bit/s'), $r);
?>
<form class="fpbx-submit" name="dmodem_settings" method="post"
      action="config.php?display=dmodem&amp;view=settings">
	<input type="hidden" name="action" value="save_settings">

	<h2><?php echo _('Engine'); ?></h2>
	<?php
	$row('engine', _('Calls go to'),
	     $select('engine', $s['engine'], [
		     'app' => _('DModem() in Asterisk (app_dmodem.so)'),
		     'ari' => _('dmodem-ari beside Asterisk (Stasis)'),
	     ]),
	     _('app_dmodem runs the modem inside Asterisk: one hop less, the recommended setup. dmodem-ari is a program of its own on Asterisk\'s ARI, for an Asterisk you\'d rather not load a module into; it reads dmodem.conf when it starts, so restart it after Apply Config.'));
	$row('debug', _('Log level'),
	     $select('debug', $s['debug'], [
		     0 => _('Errors only'), 1 => _('The modem core'),
		     2 => _('More of the modem core'), 3 => _('The data pumps too'),
	     ]),
	     _('The modem\'s log on the Asterisk console (verbose 5).'));
	?>

	<div class="dmodem-ari">
		<h2><?php echo _('dmodem-ari'); ?></h2>
		<?php
		$row('ari_url', _('ARI URL'), $text('ari_url', $s['ari_url']),
		     _('Asterisk\'s HTTP server (http.conf). FreePBX\'s is on port 8088 by default.'));
		$row('ari_user', _('ARI user'), $text('ari_user', $s['ari_user']),
		     _('A user of Settings > Asterisk REST Interface Users (ari.conf), not read only.'));
		$row('ari_password', _('ARI password'), $text('ari_password', '', 'password'),
		     _('Left empty, the one there is stays.'));
		$row('ari_app', _('Stasis app'), $text('ari_app', $s['ari_app']),
		     _('The name dmodem-ari registers; the dialplan calls Stasis(<name>,<profile>).'));
		$row('ari_t38', _('T.38 through its own SIP side'), $check('ari_t38', $s['ari_t38']),
		     _('Faxes over T.38: the calls\' lines go on legs to dmodem-ari\'s SIP side, a PJSIP endpoint with T.38 (docs/ari.md, T.38).'));
		$row('sip_endpoint', _('Its PJSIP endpoint'), $text('sip_endpoint', $s['sip_endpoint']),
		     _('The endpoint Asterisk calls dmodem-ari\'s SIP side on (t38_udptl = yes).'));
		$row('sip_bind', _('Its SIP address'), $text('sip_bind', $s['sip_bind']),
		     _('address:port dmodem-ari listens on for those legs.'));
		?>
	</div>

	<h2><?php echo _('Dialling out'); ?></h2>
	<?php
	$row('dialout_context', _('Dial-out context'), $text('dialout_context', $s['dialout_context']),
	     _('Where the number is dialled: from-internal goes through your outbound routes, as an extension\'s call would.'));
	$row('dialout_callerid', _('Caller ID'), $text('dialout_callerid', $s['dialout_callerid']),
	     _('Sent with the call: a number, or "Name" <number>. Empty: the outbound route\'s.'));
	$row('dialout_ring', _('Ring time'), $text('dialout_ring', $s['dialout_ring']),
	     _('Seconds the far end may ring before the call is given up.'));
	?>

	<h2><?php echo _('Fax'); ?></h2>
	<?php
	$row('fax_destination', _('Fax destination'), $check('fax_destination', $s['fax_destination']),
	     _('A destination "D-Modem: receive a fax" that answers as a fax machine.'));
	$row('fax_dir', _('Fax directory'), $text('fax_dir', $s['fax_dir']),
	     _('Where received faxes go, as TIFF files: <date>-<time>-<caller>-<uniqueid>.tif. Asterisk must be able to write there.'));
	$row('fax_ident', _('Our fax number'), $text('fax_ident', $s['fax_ident']),
	     _('Sent to the other fax machine (CSI/TSI): up to 20 digits, + and spaces.'));
	$row('fax_max_rate', _('Fastest fax rate'), $select('fax_max_rate', $s['fax_max_rate'], $rates),
	     _('Lower it for lines that drop pages at 14400.'));
	$row('fax_ecm', _('Error correction (ECM)'), $check('fax_ecm', $s['fax_ecm']),
	     _('Offered to the other machine: pages come through line hits undamaged.'));
	$row('fax_t38', _('T.38'), $check('fax_t38', $s['fax_t38']),
	     _('Faxes over T.38 when the channel has it (a PJSIP endpoint with T.38 on).'));
	?>
</form>
