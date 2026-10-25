// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Neo-Desktop
//
// dmodem.js - the D-Modem page (FreePBX loads it for display=dmodem):
// a profile form shows the fields of its mode, the settings those of the
// engine.

$(function () {
	// the rows of field names shown when on, hidden else
	function show(names, on) {
		names.forEach(function (n) {
			$('#dmodem [data-field="' + n + '"]').toggle(on);
		});
	}

	// the profile form's fields for its mode
	function mode() {
		var m = $('#dmodem select#mode').val();
		if (m === undefined)
			return;
		show(['host', 'port'], m === 'tcp');
		show(['path'], m === 'unix');
		show(['command'], m === 'exec');
		show(['header'], m !== 'exec');
	}

	// the settings' dmodem-ari part for the engine
	function engine() {
		var e = $('#dmodem select#engine').val();
		if (e === undefined)
			return;
		$('#dmodem .dmodem-ari').toggle(e === 'ari');
		show(['sip_endpoint', 'sip_bind'], $('#dmodem #ari_t38').is(':checked'));
	}

	$('#dmodem select#mode').on('change', mode);
	$('#dmodem select#engine, #dmodem #ari_t38').on('change', engine);
	mode();
	engine();
});
