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
		     'ari' => _('dmodem-ari beside Asterisk (Stasis)'),
		     'app' => _('DModem() in Asterisk (app_dmodem.so, built yourself)'),
	     ]),
	     _('dmodem-ari runs the modem as a program of its own on Asterisk\'s ARI, the recommended setup; it reads dmodem.conf when it starts, so restart it after Apply Config. app_dmodem runs the modem inside Asterisk, one hop less, but only as a module you build for your own Asterisk: a built app_dmodem.so may not be passed on (LICENSING.md).'));
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
	$row('fax_keep_days', _('Keep faxes'), $text('fax_keep_days', $s['fax_keep_days']),
	     _('Days that a received fax stays in the Faxes list and in the fax directory. 0: all are kept.'));
	$list = function ($k, $v) {
		return '<textarea class="form-control" rows="4" id="' . $k . '" name="' . $k . '">' .
			D::h($v) . '</textarea>';
	};
	$row('fax_block', _('Block faxes from'), $list('fax_block', $s['fax_block']),
	     _('One caller ID on each line: a number, a prefix with * at the end (for example 1900*), or anonymous for a call without a number. Text after # is a comment. A blocked fax is kept in the Faxes list as BLOCKED, and it does not go by email or to UCP.'));
	$row('fax_allow', _('Never block faxes from'), $list('fax_allow', $s['fax_allow']),
	     _('The same form. A caller ID on this list is never blocked.'));
	$row('fax_allow_only', _('Only the allow list'), $check('fax_allow_only', $s['fax_allow_only']),
	     _('Block each fax from a caller ID that is not on the "Never block" list.'));
	$row('fax_ident', _('Our fax number'), $text('fax_ident', $s['fax_ident']),
	     _('Sent to the other fax machine (CSI/TSI): up to 20 digits, + and spaces.'));
	$row('fax_header', _('Fax header'), $text('fax_header', $s['fax_header']),
	     _('A header line on the top of each page that D-Modem sends: the date, the time, this text, our fax number and the page number. Empty: no header line.'));
	$row('fax_max_rate', _('Fastest fax rate'), $select('fax_max_rate', $s['fax_max_rate'], $rates),
	     _('Lower it for lines that drop pages at 14400.'));
	$minrates = [0 => _('None')] + $rates;
	$row('fax_min_rate', _('Slowest fax rate'), $select('fax_min_rate', $s['fax_min_rate'], $minrates),
	     _('A fax that cannot go at this rate or faster is not taken. None: as slow as the other machine goes.'));
	$row('fax_ecm', _('Error correction (ECM)'), $check('fax_ecm', $s['fax_ecm']),
	     _('Offered to the other machine: pages come through line hits undamaged.'));
	$row('fax_t38', _('T.38'), $check('fax_t38', $s['fax_t38']),
	     _('Faxes over T.38 when the channel has it (a PJSIP endpoint with T.38 on).'));
	?>
	<h2><?php echo _('Sending faxes'); ?></h2>
	<?php
	$row('fax_retries', _('Tries again'), $select('fax_retries', $s['fax_retries'], [0 => '0', 1 => '1', 2 => '2', 3 => '3']),
	     _('The tries after the first, when a fax is not sent. The send page can change it for each fax.'));
	$row('fax_retry_delay', _('Minutes between tries'), $text('fax_retry_delay', $s['fax_retry_delay']),
	     _('From 1 to 120.'));
	$row('fax_notify', _('Email the result'), $select('fax_notify', $s['fax_notify'], [
		     'never' => _('Never'),
		     'always' => _('Always'),
		     'success' => _('When it is sent'),
		     'failure' => _('When it is not sent'),
	     ]),
	     _('The default of the send page.'));
	$row('fax_notify_email', _('Result to'), $text('fax_notify_email', $s['fax_notify_email']),
	     _('The default address for the result.'));
	$row('fax_ghostscript', _('Ghostscript'), $text('fax_ghostscript', $s['fax_ghostscript']),
	     _('The gs program, which makes the fax pages from PDF files (on FreePBX: yum or apt install ghostscript).'));
	$row('fax_office', _('LibreOffice'), $text('fax_office', $s['fax_office']),
	     _('The soffice program, which makes PDF files from Office documents (Word, Excel, PowerPoint, OpenDocument, RTF, text). Empty: only PDF and TIFF files.'));
	$row('fax_api', _('Send API'), $check('fax_api', $s['fax_api']),
	     _('Programs send faxes and get their state through ajax.php?module=dmodem&command=sendfax and faxstatus, with the token in the X-Dmodem-Token header.'));
	$row('fax_api_token', _('Send API token'), $text('fax_api_token', '', 'password'),
	     ($s['fax_api_token'] !== '' ? _('A token is set. Empty: keep it.') . ' ' : '') .
	     _('32 to 128 letters, digits, _ and -. For example, make one with: openssl rand -hex 32'));
	$row('fax_cover_name', _('Cover page: company'), $text('fax_cover_name', $s['fax_cover_name']),
	     _('Your company name, on the top of the cover page.'));
	$row('fax_cover_address', _('Cover page: address'),
	     '<textarea class="form-control" rows="3" id="fax_cover_address" name="fax_cover_address">' .
	     D::h($s['fax_cover_address']) . '</textarea>',
	     _('Up to 5 lines.'));
	$row('fax_cover_phone', _('Cover page: telephone'), $text('fax_cover_phone', $s['fax_cover_phone']),
	     _('For example "Tel. +1 555 0100, fax +1 555 0101".'));
	$row('fax_cover_web', _('Cover page: web site'), $text('fax_cover_web', $s['fax_cover_web']), '');
	$row('fax_cover_footer', _('Cover page: footer'), $text('fax_cover_footer', $s['fax_cover_footer']),
	     _('A line on the bottom of the cover page, for example a confidentiality notice.'));
	?>
	<h2><?php echo _('Email to fax'); ?></h2>
	<p class="text-muted"><?php echo _('A message to <number>@<your fax domain> becomes a fax to that number: its PDF, TIFF and Office files, and its text on the cover page. The mail system puts each message into the spool folder (bin/mail2fax-spool.php, through an alias with a pipe), and the queue reads the folder each minute. The result goes to the sender as Email the result says.'); ?></p>
	<?php
	$row('mail2fax', _('Email to fax'), $check('mail2fax', $s['mail2fax']), '');
	$row('mail2fax_spool', _('Spool folder'), $text('mail2fax_spool', $s['mail2fax_spool']),
	     _('The mail system writes here, and Asterisk reads and deletes the messages (for example mode 1733, owner asterisk).'));
	$row('mail2fax_domain', _('Fax domain'), $text('mail2fax_domain', $s['mail2fax_domain']),
	     _('Only the addresses in this domain give fax numbers, for example fax.example.com. Empty: all domains.'));
	$row('mail2fax_senders', _('Senders'), $list('mail2fax_senders', $s['mail2fax_senders']),
	     _('The senders that may send faxes: one address or @domain on each line. Other messages are dropped, without an answer.'));
	$row('mail2fax_pin', _('PIN'), $text('mail2fax_pin', $s['mail2fax_pin']),
	     _('If set, the subject must have it as a word: a sender address alone is easy to forge. 4 to 32 letters and digits.'));
	$row('mail2fax_cover', _('Cover page'), $check('mail2fax_cover', $s['mail2fax_cover']),
	     _('The text of the message (and the subject, without a PIN) on a cover page, with the sender.'));
	?>
	<h2><?php echo _('Fax to email'); ?></h2>
	<p class="text-muted"><?php echo _('A fax to a recipient\'s destination (the Fax recipients tab) goes to the recipient\'s email address when the call ends. In the subject and the text, these fields are replaced: {cid} {cidname} {remote} {pages} {status} {date} {recipient} {file}.'); ?></p>
	<?php
	$row('fax_attach', _('Attach'), $select('fax_attach', $s['fax_attach'], [
		     'pdf' => _('A PDF file'),
		     'tiff' => _('The TIFF file'),
		     'both' => _('Both'),
	     ]),
	     _('What goes with the message. A recipient can have its own.'));
	$row('fax_paper', _('PDF paper size'), $select('fax_paper', $s['fax_paper'], [
		     'letter' => _('Letter'),
		     'a4' => _('A4'),
	     ]),
	     _('Each page of the PDF file is of this size.'));
	$row('fax_email_from', _('From'), $text('fax_email_from', $s['fax_email_from']),
	     _('The sender of the message: an address, or Name <address>. Empty: the mail system\'s.'));
	$row('fax_email_subject', _('Subject'), $text('fax_email_subject', $s['fax_email_subject']),
	     _('The subject of the message.'));
	$row('fax_email_body', _('Text'),
	     '<textarea class="form-control" rows="4" id="fax_email_body" name="fax_email_body">' .
	     D::h($s['fax_email_body']) . '</textarea>',
	     _('The text of the message.'));
	?>
</form>
