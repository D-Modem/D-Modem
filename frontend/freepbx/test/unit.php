<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// unit.php - the FreePBX module's tests without FreePBX (fpbx_env.php):
// the forms, the store, dmodem.conf as app_dmodem and dmodem-ari read
// it, the dialplan, the destinations, the line check on Asterisk's own
// CLI output (fixtures/, from Asterisk 20) and the pages.  Exit status 0
// if all pass.  FREEPBX_FRAMEWORK=<FreePBX's framework checkout>: the
// dialplan through its own extensions class.

require_once __DIR__ . '/fpbx_env.php';

use FreePBX\modules\Dmodem;
use FreePBX\modules\Dmodem\Config;
use FreePBX\modules\Dmodem\Dialplan;
use FreePBX\modules\Dmodem\LineCheck;
use FreePBX\modules\Dmodem\Dialout;
use FreePBX\modules\Dmodem\Cli;
use FreePBX\modules\Dmodem\FaxMail;
use FreePBX\modules\Dmodem\FaxPdf;
use FreePBX\modules\Dmodem\FaxLog;
use FreePBX\modules\Dmodem\FaxSend;
use FreePBX\modules\Dmodem\FaxJunk;
use FreePBX\modules\Dmodem\FaxCover;

$failed = 0;
$passed = 0;

/* one check: what it checks and whether it holds */
function ok($cond, $what)
{
	global $failed, $passed;
	if ($cond) {
		$passed++;
	} else {
		$failed++;
		echo "FAIL: $what\n";
	}
}

/* a, b the same, else both shown */
function same($a, $b, $what)
{
	ok($a === $b, $what);
	if ($a !== $b)
		echo '  got:  ' . var_export($a, true) . "\n  want: " . var_export($b, true) . "\n";
}

/* fixture name's text */
function fixture($name)
{
	return file_get_contents(__DIR__ . '/fixtures/' . $name . '.txt');
}

/* dmodem.conf text as dmodem-ari's read_conf() reads it (and Asterisk's
   config parser for these files): section => [name => value] */
function read_conf($text)
{
	$conf = [];
	$section = 'general';
	foreach (explode("\n", $text) as $line) {
		$l = '';
		for ($i = 0; $i < strlen($line); $i++) {
			if ($line[$i] === '\\' && $i + 1 < strlen($line) && $line[$i + 1] === ';') {
				$l .= ';';
				$i++;
			} elseif ($line[$i] === ';') {
				break;
			} else {
				$l .= $line[$i];
			}
		}
		$l = trim($l);
		if ($l === '')
			continue;
		if ($l[0] === '[') {
			$section = trim(substr($l, 1, strpos($l, ']') - 1));
			$conf[$section] = isset($conf[$section]) ? $conf[$section] : [];
			continue;
		}
		$eq = strpos($l, '=');
		if ($eq === false)
			continue;
		$v = substr($l, $eq + 1);
		if ($v !== '' && $v[0] === '>')
			$v = substr($v, 1);
		$conf[$section][trim(substr($l, 0, $eq))] = trim($v);
	}
	return $conf;
}

/* --- the store and the settings --- */

$m = dmodem_new();
$s = $m->getSettings();
same($s, Config::defaultSettings(), 'install(): the default settings, typed');

dmodem_post($m, ['action' => 'save_settings', 'engine' => 'app', 'debug' => '1',
		 'fax_dir' => '/var/spool/asterisk/fax', 'fax_ident' => '+1 555 0100',
		 'fax_header' => 'ACME; Ltd', 'fax_max_rate' => '9600', 'fax_destination' => 'yes',
		 /* fax_ecm and fax_t38 unchecked: not posted */
		 'ari_url' => 'http://127.0.0.1:8088', 'ari_user' => 'dmodem',
		 'ari_password' => '', 'ari_app' => 'dmodem',
		 'sip_endpoint' => 'dmodem', 'sip_bind' => '127.0.0.1:5090']);
same($m->errors, [], 'settings saved without errors');
$s = $m->getSettings();
same($s['fax_ecm'], false, 'an unchecked box stays off (the store deletes false)');
same($s['fax_t38'], false, 'fax_t38 off');
same($s['fax_destination'], true, 'fax_destination on');
same($s['fax_max_rate'], 9600, 'fax_max_rate an int');
same($s['debug'], 1, 'debug an int');
ok($GLOBALS['needreload'] > 0, 'saving asks for Apply Config');

dmodem_post($m, ['action' => 'save_settings', 'engine' => 'nonsense', 'debug' => '9',
		 'fax_dir' => "relative\npath", 'fax_ident' => 'abc', 'fax_max_rate' => '1',
		 'fax_header' => str_repeat('x', 64)]);
foreach (['engine', 'debug', 'fax_dir', 'fax_ident', 'fax_max_rate', 'fax_header'] as $k)
	ok(isset($m->errors[$k]), "bad setting $k refused");
same($m->getSettings()['debug'], 1, 'bad settings not kept');
same($_REQUEST['view'], 'settings', 'bad settings: the form again');

dmodem_post($m, ['action' => 'save_settings', 'engine' => 'ari', 'debug' => '0',
		 'fax_dir' => '/tmp', 'fax_max_rate' => '14400',
		 'ari_url' => 'ftp://x', 'ari_user' => '', 'ari_app' => 'a b',
		 'ari_t38' => 'yes', 'sip_endpoint' => '', 'sip_bind' => 'nowhere']);
foreach (['ari_url', 'ari_user', 'ari_app', 'sip_endpoint', 'sip_bind'] as $k)
	ok(isset($m->errors[$k]), "bad ARI setting $k refused");

/* --- profiles --- */

$bbs = ['action' => 'save_profile', 'original' => '', 'name' => 'BBS',
	'description' => 'The <b>BBS</b>', 'mode' => 'tcp', 'host' => '127.0.0.1',
	'port' => '2323', 'law' => 'ulaw', 'init' => '', 'answer' => 'AT+MS=132;A',
	'timeout' => '60', 'fax' => 'yes'];
dmodem_post($m, $bbs);
same($m->errors, [], 'profile bbs saved');
$p = $m->getProfile('bbs');
ok($p !== null, 'the name lowercased: bbs');
same($p['port'], 2323, 'port an int');
same($p['header'], false, 'header unchecked: off');
same($p['fax'], true, 'fax checked: on');

dmodem_post($m, ['action' => 'save_profile', 'original' => '', 'name' => 'ppp',
		 'mode' => 'exec', 'command' => 'exec /usr/sbin/pppd nodetach noauth local; echo x',
		 'law' => 'auto', 'timeout' => '45', 'voice' => '7000',
		 'voice_dsvd' => 'open']);
same($m->errors, [], 'profile ppp saved');

$bad = [
	'name' => ['name' => '-x'],
	'name ' => ['name' => 'general'],
	'name  ' => ['name' => 'receivefax'],
	'name   ' => ['name' => 'bbs'],
	'port' => ['port' => '70000'],
	'host' => ['host' => 'a b'],
	'mode' => ['mode' => 'udp'],
	'init' => ['init' => 'ATZ' . "\n" . 'ATH'],
	'answer' => ['answer' => 'hello'],
	'timeout' => ['timeout' => '0'],
	'law' => ['law' => 'g729'],
	'clear' => ['clear' => 'maybe'],
	'description' => ['description' => "two\nlines"],
	'voice' => ['voice' => '70 00'],
	'voice ' => ['voice' => '7000@a b'],
	'voice_listen' => ['voice' => '7000', 'voice_listen' => '31'],
	'voice_dsvd' => ['voice' => '7000', 'voice_dsvd' => 'maybe'],
	'voice_dsvd ' => ['voice_dsvd' => 'yes'],
];
foreach ($bad as $field => $over) {
	dmodem_post($m, array_merge($bbs, ['name' => 'other'], $over));
	ok(isset($m->errors[trim($field)]), 'bad profile field ' . trim($field) . ' refused');
	ok($m->getProfile('other') === null, 'a bad profile is not kept (' . trim($field) . ')');
}
dmodem_post($m, array_merge($bbs, ['name' => 'sock', 'mode' => 'unix', 'path' => 'relative']));
ok(isset($m->errors['path']), 'a unix socket path must be absolute');
dmodem_post($m, array_merge($bbs, ['name' => 'run', 'mode' => 'exec', 'command' => '']));
ok(isset($m->errors['command']), 'exec needs a command');
dmodem_post($m, array_merge($bbs, ['name' => 'hand', 'handoff' => 'yes', 'header' => 'yes']));
ok(isset($m->errors['handoff']), 'handoff needs a unix socket');
dmodem_post($m, array_merge($bbs, ['name' => 'hand', 'mode' => 'unix', 'path' => '/run/menu.sock',
				   'handoff' => 'yes']));
ok(isset($m->errors['handoff']), 'handoff needs the call details first');
ok($m->getProfile('hand') === null, 'a bad handoff profile is not kept');
dmodem_post($m, array_merge($bbs, ['name' => 'menu', 'mode' => 'unix', 'path' => '/run/menu.sock',
				   'header' => 'yes', 'handoff' => 'yes']));
same($m->errors, [], 'profile menu (handoff) saved');
same($m->getProfile('menu')['handoff'], true, 'handoff checked: on');

/* editing keeps the name: it is the destination */
dmodem_post($m, array_merge($bbs, ['original' => 'bbs', 'name' => 'renamed', 'port' => '2424']));
same($m->errors, [], 'profile bbs edited');
ok($m->getProfile('renamed') === null, 'an edit does not rename');
same($m->getProfile('bbs')['port'], 2424, 'the edit kept');
dmodem_post($m, array_merge($bbs, ['original' => 'bbs', 'port' => '2323']));

/* --- dmodem.conf --- */

$m->writeConfig($m->genConfig());
$conf = $m->FreePBX->written['dmodem.conf'];
$c = read_conf($conf);
same(array_keys($c), ['general', 'bbs', 'menu', 'ppp'], 'dmodem.conf: [general] and the profiles in order');
same($c['bbs']['answer'], 'AT+MS=132;A', 'a ; in a value survives (written \\;)');
same($c['ppp']['command'], 'exec /usr/sbin/pppd nodetach noauth local; echo x', 'the command with its ;');
same($c['bbs']['host'] . ':' . $c['bbs']['port'], '127.0.0.1:2323', 'tcp host and port');
same($c['bbs']['law'], 'ulaw', 'law');
same($c['bbs']['clear'], 'auto', 'clear: auto by default');
same($c['bbs']['fax'], 'yes', 'fax yes');
same($c['general']['fax_ecm'], 'no', 'fax_ecm no');
same($c['general']['fax_ident'], '+1 555 0100', 'fax_ident');
same($c['general']['fax_header'], 'ACME; Ltd', 'fax_header (its ; written \\;)');
same($c['general']['fax_max_rate'], '9600', 'fax_max_rate');
ok(!isset($c['ppp']['header']), 'exec has no header');
same($c['ppp']['voice'] . ' ' . $c['ppp']['voice_listen'] . ' ' . $c['ppp']['voice_dsvd'],
     '7000@from-internal 3 open', 'voice: a bare extension in from-internal, listen 3 s, DSVD open');
ok(!isset($c['bbs']['voice']) && !isset($c['bbs']['voice_dsvd']), 'no voice extension: no voice lines');
same($c['menu']['handoff'] . ' ' . $c['menu']['header'], 'yes yes', 'handoff and header on the unix profile');
ok(!isset($c['bbs']['handoff']) && !isset($c['ppp']['handoff']), 'handoff only for unix');
dmodem_post($m, ['action' => 'delete', 'profile' => 'menu']);
ok($m->getProfile('menu') === null, 'profile menu deleted');
ok(!isset($c['general']['ari_url']), 'no ARI settings for app_dmodem');
ok(strpos($conf, '<b>') !== false && strpos($conf, "\n; The <b>BBS</b>\n") !== false,
   'the description as a comment');

/* line breaks can't sneak in a setting, even past the checks */
$x = Config::render(Config::defaultSettings(), ['evil' => array_merge(Config::defaultProfile(),
	['mode' => 'exec', 'command' => "true\n[general]\ndebug = 3"])]);
$xc = read_conf($x);
same($xc['general']['debug'], '0', 'no section injected through a value');

/* --- the dialplan --- */

$dp = dmodem_dialplan($m);
ok(strpos($dp, "[app-dmodem]\n") !== false, 'the app-dmodem context');
ok(strpos($dp, "exten => bbs,1,NoOp(D-Modem profile bbs)\n") !== false, 'bbs starts at 1');
ok(strpos($dp, "exten => bbs,n,DModem(bbs)\n") !== false, 'bbs runs DModem(bbs)');
ok(strpos($dp, "exten => ppp,n,DModem(ppp)\n") !== false, 'ppp runs DModem(ppp)');
ok(strpos($dp, "exten => bbs,n,Set(CHANNEL(hangup_handler_push)=app-dmodem-fax2mail,s,1())\n") !== false,
   'bbs (with fax) pushes the fax log\'s handler');
ok(strpos($dp, "exten => receivefax,n,Set(CHANNEL(hangup_handler_push)=app-dmodem-fax2mail,s,1())\n") !== false,
   'receivefax pushes the fax log\'s handler');
ok(strpos($dp, "[app-dmodem-fax2mail]\n") !== false, 'the fax log\'s handler without recipients');
ok(strpos($dp, "exten => receivefax,n,DModemReceiveFax(/var/spool/asterisk/fax/\${STRFTIME(\${EPOCH},,%Y%m%d-%H%M%S)}-\${FILTER(0-9A-Za-z+,\${CALLERID(num)})}-\${UNIQUEID}.tif)\n") !== false,
   'receivefax runs DModemReceiveFax into fax_dir');
same(substr_count($dp, ',n,Hangup()'), 5, 'each extension hangs up (and the sent fax\'s s and failed)');
echo DMODEM_TEST_REAL_EXTENSIONS ? "(FreePBX's own extensions class)\n" : '';

/* the ARI engine: Stasis() (on a store of its own: m's put back after) */
$saved = unserialize(serialize(FreePBX_Helpers::$kv));
$m2 = dmodem_new();
dmodem_post($m2, array_merge($bbs, ['original' => '']));
dmodem_post($m2, ['action' => 'save_settings', 'engine' => 'ari', 'debug' => '0',
		  'fax_dir' => '/tmp/fax', 'fax_max_rate' => '14400', 'fax_destination' => 'yes',
		  'ari_url' => 'http://127.0.0.1:8088', 'ari_user' => 'dmodem',
		  'ari_password' => 'se;cret', 'ari_app' => 'dm',
		  'sip_endpoint' => 'dmodem', 'sip_bind' => '127.0.0.1:5090']);
same($m2->errors, [], 'ARI settings saved');
$dp2 = dmodem_dialplan($m2);
ok(strpos($dp2, "exten => bbs,n,Stasis(dm,bbs)\n") !== false, 'ari: Stasis(<app>,<profile>)');
ok(strpos($dp2, 'Stasis(dm,receivefax,/tmp/fax/') !== false, 'ari: Stasis(<app>,receivefax,<file>)');
$c2 = read_conf($m2->genConfig()['dmodem.conf']);
same($c2['general']['ari_password'], 'se;cret', 'the ARI password with its ;');
same($c2['general']['ari_app'], 'dm', 'ari_app');
ok(!isset($c2['general']['t38']), 'no dmodem-ari T.38 unless asked');
/* an empty password keeps the one there is */
dmodem_post($m2, ['action' => 'save_settings', 'engine' => 'ari', 'debug' => '0',
		  'fax_dir' => '/tmp/fax', 'fax_max_rate' => '14400',
		  'ari_url' => 'http://127.0.0.1:8088', 'ari_user' => 'dmodem',
		  'ari_password' => '', 'ari_app' => 'dm', 'ari_t38' => 'yes',
		  'sip_endpoint' => 'dmodem', 'sip_bind' => '127.0.0.1:5090']);
$c2 = read_conf($m2->genConfig()['dmodem.conf']);
same($c2['general']['ari_password'], 'se;cret', 'the ARI password kept');
same($c2['general']['t38'] . ' ' . $c2['general']['sip_bind'], 'yes 127.0.0.1:5090', 'dmodem-ari T.38');
ok(strpos(dmodem_dialplan($m2), 'receivefax') === false, 'no fax destination when it is off');
FreePBX_Helpers::$kv = $saved;
FreePBX::$modules['Dmodem'] = $m;

/* --- destinations --- */

$d = dmodem_destinations();
same(array_column($d, 'destination'), ['app-dmodem,bbs,1', 'app-dmodem,ppp,1', 'app-dmodem,receivefax,1'],
     'a destination for each profile and the fax');
same($d[0]['description'], 'bbs (The <b>BBS</b>)', 'the destination named with its description');
same(dmodem_getdest('bbs'), ['app-dmodem,bbs,1'], 'getdest');
same(dmodem_getdestinfo('app-dmodem,bbs,1')['edit_url'],
     'config.php?display=dmodem&view=form&profile=bbs', 'getdestinfo: the edit page');
same(dmodem_getdestinfo('app-dmodem,gone,1'), [], 'getdestinfo of a gone profile: empty (FreePBX flags it)');
same(dmodem_getdestinfo('ext-local,100,1'), null, 'getdestinfo of another module: null');

/* deleting a profile in use is refused */
$GLOBALS['dest_usage']['app-dmodem,ppp,1'] = ['core' => ['inbound route 5551000']];
dmodem_post($m, ['action' => 'delete', 'profile' => 'ppp']);
ok($m->getProfile('ppp') !== null, 'a profile in use is not deleted');
same($m->message[0], 'danger', 'and the page says why');
$GLOBALS['dest_usage'] = [];
dmodem_post($m, ['action' => 'delete', 'profile' => 'ppp']);
ok($m->getProfile('ppp') === null, 'a profile not in use is deleted');

/* --- the line check, on Asterisk 20's output --- */

same(LineCheck::endpointNames(fixture('pjsip_show_endpoints')), ['bad', 'good'],
     'the endpoints of "pjsip show endpoints" (not its header)');
$good = LineCheck::endpointSettings(fixture('pjsip_show_endpoint_good'));
same($good['allow'], ['ulaw'], 'good: its codecs');
same($good['dtmf_mode'], 'rfc4733', 'good: its DTMF mode');
$rg = LineCheck::evaluate($good);
same(LineCheck::worst($rg), 'ok', 'good: nothing to fix');
$bad = LineCheck::endpointSettings(fixture('pjsip_show_endpoint_bad'));
same($bad['allow'], ['ulaw', 'alaw', 'g722', 'gsm'], 'bad: its codecs');
$rb = LineCheck::evaluate($bad);
same(LineCheck::worst($rb), 'warn', 'bad: things to fix');
$texts = implode("\n", array_column($rb, 'text'));
foreach (['Other codecs allowed besides G.711 (g722, gsm)', 'Both ulaw and alaw',
	  'DTMF mode inband', 'Fax detection on', 'Direct media on', 'T.38 on'] as $want)
	ok(strpos($texts, $want) !== false, "bad: \"$want\"");
same(LineCheck::worst(LineCheck::evaluate(['allow' => ['g729']])), 'warn', 'no G.711 at all: fix');
same(LineCheck::worst(LineCheck::evaluate(['allow' => ['alaw', 'clearmode']])), 'info',
     'CLEARMODE beside G.711: not another codec, but said');
same(LineCheck::worst(LineCheck::evaluate(['allow' => ['clearmode']])), 'info',
     'CLEARMODE alone: ISDN data calls only');
ok(LineCheck::moduleRunning(fixture('module_show_like_app_dmodem'), 'app_dmodem'), 'app_dmodem running');
ok(!LineCheck::moduleRunning("Module  Description  Use Count  Status\n0 modules loaded\n", 'app_dmodem'),
   'app_dmodem not loaded');
ok(LineCheck::ariAppRegistered(fixture('ari_show_apps_dmodem'), 'dmodem'), 'dmodem-ari registered');
ok(!LineCheck::ariAppRegistered(fixture('ari_show_apps_none'), 'dmodem'), 'dmodem-ari not registered');

/* the check page's data through the manager */
$am = $m->FreePBX->astman;
$am->out = [
	'module show like app_dmodem' => fixture('module_show_like_app_dmodem'),
	'pjsip show endpoints' => fixture('pjsip_show_endpoints'),
	'pjsip show endpoint good' => fixture('pjsip_show_endpoint_good'),
	'pjsip show endpoint bad' => fixture('pjsip_show_endpoint_bad'),
];
$ck = $m->check($m->getSettings());
ok($ck["manager"] && $ck["engine_ok"], "check: the manager up, app_dmodem loaded");
same(array_keys($ck['endpoints']), ['bad', 'good'], 'check: each endpoint');
same($ck['endpoints']['good']['worst'] . ' ' . $ck['endpoints']['bad']['worst'], 'ok warn', 'check: their verdicts');
$am->up = false;
$ck = $m->check($m->getSettings());
ok(!$ck['manager'], 'check: no manager');
$am->up = true;

/* --- the pages --- */

$h = dmodem_get($m, []);
ok(strpos($h, 'The &lt;b&gt;BBS&lt;/b&gt;') !== false && strpos($h, 'The <b>BBS</b>') === false,
   'profiles page: the description escaped');
ok(strpos($h, 'app-dmodem,bbs,1') !== false, 'profiles page: the destination shown');
$h = dmodem_get($m, ['view' => 'form', 'profile' => 'bbs']);
ok(strpos($h, 'value="AT+MS=132;A"') !== false, 'profile form: its values');
ok(strpos($h, 'data-fpbx-delete="config.php?display=dmodem&amp;action=delete&amp;profile=bbs"') !== false,
   'profile form: the delete URL');
ok(strpos($h, 'readonly') !== false, 'profile form: the name read only when editing');
$h = dmodem_get($m, ['view' => 'form']);
ok(strpos($h, 'New profile') !== false, 'the new profile form');
$h = dmodem_get($m, ['view' => 'settings']);
ok(strpos($h, 'name="fax_dir"') !== false && strpos($h, 'value="se;cret"') === false,
   'settings page (the password not shown)');
$h = dmodem_get($m, ['view' => 'check']);
ok(strpos($h, 'app_dmodem is loaded') !== false && strpos($h, 'Fax detection on') !== false,
   'check page');
$h = dmodem_get($m, ['view' => 'dialplan']);
ok(strpos($h, 'DModem(bbs)') !== false && strpos($h, '[general]') !== false, 'generated files page');
/* a bad post shows the form with its values and errors */
dmodem_post($m, array_merge($bbs, ['name' => 'new', 'port' => 'x']));
$h = $m->showPage();
ok(strpos($h, 'value="x"') !== false && strpos($h, 'A port from 1 to 65535') !== false,
   'a refused form again, with its values and the error');

same(array_keys($m->getActionBar(['display' => 'dmodem', 'view' => 'form', 'profile' => 'bbs'])),
     ['reset', 'submit', 'delete'], 'action bar: an existing profile');
same(array_keys($m->getActionBar(['display' => 'dmodem', 'view' => 'form'])),
     ['reset', 'submit'], 'action bar: a new profile');
same($m->getActionBar(['display' => 'dmodem']), [], 'action bar: none on the list');

/* --- dialling out --- */

$o = Dialout::originate(Config::defaultSettings(), 'bbs', '15551234');
same($o['Application'] . '(' . $o['Data'] . ')', 'Stasis(dmodem,bbs,o)',
     'Originate: dmodem-ari\'s Stasis() by default');
$o = Dialout::originate(array_merge(Config::defaultSettings(), ['engine' => 'app']),
			'bbs', '15551234');
same($o, ['Channel' => 'Local/15551234@from-internal', 'Application' => 'DModem',
	  'Data' => 'bbs,o', 'Timeout' => '60000', 'Async' => 'true',
	  'Variable' => 'DMODEM_DIALOUT=bbs'], 'Originate: DModem(bbs,o) on Local/<number>@from-internal');
$o = Dialout::originate(array_merge(Config::defaultSettings(), ['engine' => 'ari',
	'ari_app' => 'dm', 'dialout_context' => 'modem-out', 'dialout_ring' => 30,
	'dialout_callerid' => '5550100']), 'bbs', '*99#');
same([$o['Channel'], $o['Application'], $o['Data'], $o['Timeout'], $o['CallerID']],
     ['Local/*99#@modem-out', 'Stasis', 'dm,bbs,o', '30000', '5550100'],
     'Originate for dmodem-ari: Stasis(<app>,<profile>,o), the settings\' context, ring and caller ID');
same(Dialout::originate(Config::defaultSettings(), 'bbs', '1', '"BBS" <5550199>')['CallerID'],
     '"BBS" <5550199>', 'a caller ID given wins');
$profiles = $m->getProfiles();
ok(isset(Dialout::validate('nope', '1', '', $profiles)['profile']), 'dial: an unknown profile');
foreach (['', '12a3', "1\r\nAction: Command", str_repeat('1', 41)] as $n)
	ok(isset(Dialout::validate('bbs', $n, '', $profiles)['number']), 'dial: bad number ' . json_encode($n));
foreach (["5550100\r\nAction: Logoff", '"a"b" <1>', 'name <1>'] as $c)
	ok(isset(Dialout::validate('bbs', '1', $c, $profiles)['callerid']), 'dial: bad caller ID ' . json_encode($c));
same(Dialout::validate('bbs', '15551234', '"The BBS" <5550199>', $profiles), [], 'dial: good arguments');

$am = $m->FreePBX->astman;
$am->sent = [];
$r = $m->dialOut('bbs', '15551234');
ok($r['ok'], 'dialOut: queued');
same(count($am->sent), 1, 'dialOut: one manager action');
same($am->sent[0][0], 'Originate', 'dialOut: an Originate');
same($am->sent[0][1]['Channel'], 'Local/15551234@from-internal', 'dialOut: through the outbound routes');
$r = $m->dialOut('bbs', '1x');
ok(!$r['ok'] && count($am->sent) === 1, 'dialOut: bad number, nothing sent');
$am->reply = ['Response' => 'Error', 'Message' => 'Extension does not exist.'];
$r = $m->dialOut('bbs', '15551234');
ok(!$r['ok'] && strpos($r['message'], 'Extension does not exist.') !== false, 'dialOut: Asterisk\'s refusal shown');
$am->reply = ['Response' => 'Success'];
$am->up = false;
ok(!$m->dialOut('bbs', '15551234')['ok'], 'dialOut: no manager');
$am->up = true;

/* the page's form */
dmodem_post($m, ['action' => 'dialout', 'profile' => 'bbs', 'number' => '15551234', 'callerid' => '']);
same($m->message[0], 'success', 'dial-out form: queued');
$h = $m->showPage();
ok(strpos($h, 'Calling 15551234 with profile bbs') !== false && strpos($h, 'value="15551234"') !== false,
   'dial-out page: the message and the number kept');
dmodem_post($m, ['action' => 'dialout', 'profile' => 'bbs', 'number' => 'abc', 'callerid' => '']);
$h = $m->showPage();
ok($m->message[0] === 'danger' && strpos($h, 'The number to dial') !== false, 'dial-out form: a bad number shown');
$h = dmodem_get($m, ['view' => 'dialout']);
ok(strpos($h, 'name="number"') !== false && strpos($h, 'fwconsole dmodem dial') !== false, 'dial-out page');
$h = dmodem_get($m, ['view' => 'settings']);
ok(strpos($h, 'name="dialout_context"') !== false, 'settings: the dial-out section');
$set = $m->getSettings();
foreach (['dialout_context' => 'from internal', 'dialout_callerid' => "1\n2", 'dialout_ring' => 0] as $k => $v)
	ok(isset(Config::validateSettings(array_merge($set, [$k => $v]))[$k]), "bad setting $k refused");

/* fwconsole dmodem */
$lines = [];
$say = function ($l) use (&$lines) {
	$lines[] = $l;
};
$am->sent = [];
same(Cli::run($m, ['dial', 'bbs', '15551234'], ['callerid' => '5550100'], $say), 0, 'fwconsole dmodem dial');
same($am->sent[0][1]['CallerID'], '5550100', 'fwconsole dmodem dial --callerid');
same(Cli::run($m, ['dial', 'bbs'], [], $say), 2, 'fwconsole dmodem dial: usage');
same(Cli::run($m, ['dial', 'bbs', 'x'], [], $say), 1, 'fwconsole dmodem dial: a bad number fails');
$lines = [];
Cli::run($m, ['profiles'], [], $say);
ok(count($lines) === 1 && strpos($lines[0], 'bbs') === 0, 'fwconsole dmodem profiles');
same(Cli::run($m, ['nonsense'], [], $say), 2, 'fwconsole dmodem: usage');

/* --- fax to email: the PDF, the message, the recipients --- */

/* a little-endian TIFF of one page: tags tag => [type, values] (3
   SHORT, 4 LONG, 5 RATIONAL as [num, den] pairs), its one strip data */
function tiff_page(array $tags, $data)
{
	$tags[273] = [4, [0]];
	$tags[279] = [4, [strlen($data)]];
	ksort($tags);
	$n = count($tags);
	$extra = '';
	$extraAt = 8 + 2 + 12 * $n + 4;
	$ifd = pack('v', $n);
	$stripEntry = 0;
	foreach ($tags as $tag => $tv) {
		list($type, $vals) = $tv;
		if ($tag === 273)
			$stripEntry = strlen($ifd);
		if ($type === 5) {
			$ifd .= pack('vvVV', $tag, 5, count($vals), $extraAt + strlen($extra));
			foreach ($vals as $pair)
				$extra .= pack('VV', $pair[0], $pair[1]);
		} else {
			$v = $type === 3 ? pack('v', $vals[0]) . "\0\0" : pack('V', $vals[0]);
			$ifd .= pack('vvV', $tag, $type, 1) . $v;
		}
	}
	$ifd .= pack('V', 0);
	$dataAt = $extraAt + strlen($extra);
	/* the strip's offset now that it is known */
	$ifd = substr_replace($ifd, pack('V', $dataAt), $stripEntry + 8, 4);
	return 'II' . pack('vV', 42, 8) . $ifd . $extra . $data;
}

/* bits (a string of 0 and 1) as bytes, the first bit the high one,
   0 to fill the last byte */
function bits_bytes($bits)
{
	$bits .= str_repeat('0', (8 - strlen($bits) % 8) % 8);
	$out = '';
	foreach (str_split($bits, 8) as $b)
		$out .= chr(bindec($b));
	return $out;
}

/* an MH (T.4 1-D) page of rows white rows, 1728 pels, as a fax machine
   writes it: EOL before each row, RTC at the end */
function mh_white_tiff($rows)
{
	$eol = '000000000001';
	$bits = '';
	for ($i = 0; $i < $rows; $i++)
		$bits .= $eol . '010011011' . '00110101';
	$bits .= str_repeat($eol, 6);
	return tiff_page([256 => [3, [1728]], 257 => [3, [$rows]], 258 => [3, [1]],
		259 => [3, [3]], 262 => [3, [0]], 278 => [3, [$rows]],
		282 => [5, [[204, 1]]], 283 => [5, [[98, 1]]], 292 => [4, [0]],
		296 => [3, [2]]], bits_bytes($bits));
}

/* what is wrong with PDF p's structure (its xref to each object), ''
   if nothing */
function pdf_problem($p)
{
	if (strpos($p, '%PDF-1.4') !== 0)
		return 'no header';
	if (!preg_match('/startxref\n([0-9]+)\n%%EOF\n$/', $p, $m))
		return 'no startxref';
	$x = (int)$m[1];
	if (substr($p, $x, 5) !== 'xref\n' && substr($p, $x, 4) !== 'xref')
		return 'startxref not at the xref';
	if (!preg_match('/^xref\n0 ([0-9]+)\n/', substr($p, $x), $h))
		return 'a bad xref';
	$lines = explode("\n", substr($p, $x + strlen($h[0])));
	for ($id = 1; $id < (int)$h[1]; $id++) {
		$off = (int)substr($lines[$id], 0, 10);
		if (strpos(substr($p, $off, 20), $id . ' 0 obj') !== 0)
			return "object $id not at its offset";
	}
	foreach (['/Type /Catalog', '/Type /Pages'] as $want)
		if (strpos($p, $want) === false)
			return "no $want";
	return '';
}

$mh = mh_white_tiff(4);
$pages = FaxPdf::pages($mh);
same(count($pages), 1, 'FaxPdf: one page');
same([$pages[0][0], $pages[0][1], $pages[0][2], $pages[0][3]], [1728, 4, 204.0, 98.0],
     'FaxPdf: the page\'s size and resolution');
$pdf = FaxPdf::fromTiff($mh, 'a4');
same(pdf_problem($pdf), '', 'FaxPdf: a PDF of good structure');
ok(strpos($pdf, '/Filter /CCITTFaxDecode /DecodeParms << /K 0 /Columns 1728 /EndOfLine true /Rows 4 >>') !== false,
   'FaxPdf: the MH strip as it is (CCITTFaxDecode, K 0)');
ok(strpos($pdf, '/MediaBox [0 0 595.28 841.89]') !== false, 'FaxPdf: A4 paper');
ok(strpos(FaxPdf::fromTiff($mh), '/MediaBox [0 0 612 792]') !== false, 'FaxPdf: Letter by default');
ok(strpos($pdf, "stream\n" . bits_bytes('0000000000010100')) !== false, 'FaxPdf: the coded data untouched');
/* MMR, and T.4 2-D with byte-aligned EOLs */
$t6 = tiff_page([256 => [3, [1728]], 257 => [3, [2]], 259 => [3, [4]], 262 => [3, [0]]], "\x00\x10\x01");
ok(strpos(FaxPdf::fromTiff($t6), '<< /K -1 /Columns 1728 /Rows 2 >>') !== false, 'FaxPdf: MMR (K -1)');
$t4 = tiff_page([256 => [3, [1728]], 257 => [3, [2]], 259 => [3, [3]], 262 => [3, [0]],
		 292 => [4, [5]]], "\x00\x01");
ok(strpos(FaxPdf::fromTiff($t4), '/K 1 /Columns 1728 /EndOfLine true /EncodedByteAlign true') !== false,
   'FaxPdf: T.4 2-D with fill bits (K 1, EncodedByteAlign)');
/* fill order 2: each byte's bits the other way round */
$f2 = tiff_page([256 => [3, [8]], 257 => [3, [1]], 259 => [3, [1]], 262 => [3, [1]],
		 266 => [3, [2]]], "\x01");
ok(strpos(FaxPdf::fromTiff($f2), "stream\n\x80\nendstream") !== false, 'FaxPdf: fill order 2 reversed');
/* uncompressed: 1 bit with 0 white, and colour */
$u1 = tiff_page([256 => [3, [16]], 257 => [3, [2]], 259 => [3, [1]], 262 => [3, [0]]], "\xff\xff\x00\x00");
ok(strpos(FaxPdf::fromTiff($u1), '/BitsPerComponent 1 /Decode [1 0]') !== false,
   'FaxPdf: uncompressed, photometric 0 (0 white) decoded');
$rgb = tiff_page([256 => [3, [2]], 257 => [3, [1]], 258 => [3, [8]], 259 => [3, [1]],
		  262 => [3, [2]], 277 => [3, [3]]], "\xff\x00\x00\x00\x00\xff");
ok(strpos(FaxPdf::fromTiff($rgb), '/ColorSpace /DeviceRGB /BitsPerComponent 8') !== false,
   'FaxPdf: a colour page');
/* what it does not take */
foreach (['not a TIFF' => 'GIF89a......',
	  'JPEG coding' => tiff_page([256 => [3, [8]], 257 => [3, [8]], 259 => [3, [7]]], 'x'),
	  'a strip past the end' => substr($mh, 0, strlen($mh) - 10),
	  'no size' => tiff_page([256 => [3, [0]], 257 => [3, [1]]], 'x')] as $what => $b) {
	$threw = false;
	try {
		FaxPdf::fromTiff($b);
	} catch (\RuntimeException $e) {
		$threw = true;
	}
	ok($threw, "FaxPdf: refuses $what");
}
/* rendered, when Poppler is there: white, with the black rows black */
$pdftoppm = trim((string)@shell_exec('command -v pdftoppm 2>/dev/null'));
if ($pdftoppm !== '') {
	$tmpd = sys_get_temp_dir() . '/dmodem-pdf-' . getmypid();
	@mkdir($tmpd);
	file_put_contents("$tmpd/u.pdf", FaxPdf::fromTiff(tiff_page([256 => [3, [64]],
		257 => [3, [64]], 259 => [3, [1]], 262 => [3, [0]], 282 => [5, [[8, 1]]],
		283 => [5, [[8, 1]]]], str_repeat("\xff", 256) . str_repeat("\x00", 256))));
	shell_exec('pdftoppm -gray -r 9 ' . escapeshellarg("$tmpd/u.pdf") . ' ' . escapeshellarg("$tmpd/u"));
	$pgm = glob("$tmpd/u*.pgm");
	$px = $pgm ? file_get_contents($pgm[0]) : '';
	/* an 8x8 inch image on Letter at 9 dpi: the top half black */
	ok($px !== '' && strpos($px, "\x00\x00\x00\x00") !== false && strpos($px, "\xff\xff\xff\xff") !== false,
	   'FaxPdf: Poppler renders it, black and white');
	array_map('unlink', glob("$tmpd/*"));
	rmdir($tmpd);
} else {
	echo "(no pdftoppm: the PDF not rendered)\n";
}

/* the message */
same(FaxMail::fill('{cid} {pages} {nope} {recipient}', ['cid' => '555', 'pages' => 2]),
     '555 2 {nope} ', 'fill: the fields, an unknown one left alone');
foreach (['a@b.co' => true, "a@b.co\r\nBcc: x@y.z" => false, 'A <a@b.co>' => false,
	  'a@b' => false, '' => false] as $a => $want)
	same(FaxMail::addressOk($a), $want, 'addressOk ' . json_encode($a));
list($hd, $bd, $sj) = FaxMail::build('Fax <fax@example.com>', "Fax \xc3\xa9\r\nBcc: x@y.z", 'Hello',
	[['fax 1.pdf', 'application/pdf', '%PDF']], 'BOUND');
ok(strpos($hd, "From: Fax <fax@example.com>\r\nMIME-Version: 1.0\r\n" .
	   "Content-Type: multipart/mixed; boundary=\"BOUND\"") === 0, 'build: the headers');
same($sj, '=?UTF-8?B?' . base64_encode("Fax \xc3\xa9 Bcc: x@y.z") . '?=',
     'build: a subject not ASCII encoded, on one line');
ok(strpos($bd, "Content-Disposition: attachment; filename=\"fax_1.pdf\"\r\n\r\n" .
	   base64_encode('%PDF') . "\r\n--BOUND--\r\n") !== false, 'build: the attachment, its name made safe');
ok(strpos($bd, base64_encode('Hello')) !== false, 'build: the text');

$faxdir = sys_get_temp_dir() . '/dmodem-fax-' . getmypid();
@mkdir($faxdir);
$faxfile = "$faxdir/20261127-101500-5550100-1700000000.5.tif";
file_put_contents($faxfile, mh_white_tiff(4));
same(FaxMail::findFile($faxdir, '1700000000.5'), $faxfile, 'findFile: by the unique ID');
same(FaxMail::findFile($faxdir, '1700000000.6'), null, 'findFile: none for another call');
same(FaxMail::findFile($faxdir, '*'), null, 'findFile: no glob in the unique ID');

$fs = array_merge(Config::defaultSettings(), ['fax_dir' => $faxdir,
	'fax_email_from' => 'fax@example.com']);
$mails = [];
$sender = function ($to, $subject, $body, $headers) use (&$mails) {
	$mails[] = compact('to', 'subject', 'body', 'headers');
	return true;
};
/* the parts of a mail: content type => decoded bytes */
$parts = function ($mail) {
	preg_match('/boundary="([^"]+)"/', $mail['headers'], $b);
	$out = [];
	foreach (explode('--' . $b[1], $mail['body']) as $p) {
		if (!preg_match('/Content-Type: ([a-z\/]+)/', $p, $t))
			continue;
		$split = strpos($p, "\r\n\r\n");
		$out[$t[1]] = base64_decode(substr($p, $split + 4));
	}
	return $out;
};
$vars = ['status' => 'SUCCESS', 'pages' => '1', 'cid' => '5550100', 'remote' => '+1 555 0100',
	 'cidname' => 'ACME'];
$r = FaxMail::send($fs, ['email' => 'sales@example.com', 'name' => 'Sales', 'attach' => ''],
		   '1700000000.5', $vars, $sender);
ok($r[0] && strpos($r[1], 'mailed to sales@example.com') !== false, 'send: mailed');
same($mails[0]['to'], 'sales@example.com', 'send: to the recipient');
same($mails[0]['subject'], 'Fax from 5550100 (1 pages)', 'send: the subject from its template');
$pp = $parts($mails[0]);
same(array_keys($pp), ['text/plain', 'application/pdf'], 'send: the text and a PDF (the default)');
same(pdf_problem($pp['application/pdf']), '', 'send: the PDF attached whole');
ok(strpos($pp['text/plain'], 'A fax came from 5550100 (fax number +1 555 0100): 1 pages, SUCCESS.') === 0 &&
   strpos($pp['text/plain'], 'for Sales.') !== false, 'send: the text from its template');
ok(strpos($mails[0]['body'], 'filename="fax-5550100.pdf"') !== false, 'send: the PDF named by the caller');
FaxMail::send($fs, ['email' => 'a@example.com', 'name' => 'A', 'attach' => 'both'],
	      '1700000000.5', $vars, $sender);
same(array_keys($parts($mails[1])), ['text/plain', 'application/pdf', 'image/tiff'],
     'send: both, as the recipient has it');
same($parts($mails[1])['image/tiff'], file_get_contents($faxfile), 'send: the TIFF as it is');
FaxMail::send(array_merge($fs, ['fax_attach' => 'tiff']), ['email' => 'a@example.com', 'name' => 'A'],
	      '1700000000.5', $vars, $sender);
same(array_keys($parts($mails[2])), ['text/plain', 'image/tiff'], 'send: the TIFF, as the settings have it');
/* a TIFF that FaxPdf does not take goes as the TIFF, and the text says why */
file_put_contents("$faxdir/x-1700000000.7.tif", tiff_page([256 => [3, [8]], 257 => [3, [8]],
	259 => [3, [7]]], 'x'));
FaxMail::send($fs, ['email' => 'a@example.com', 'name' => 'A'], '1700000000.7', $vars, $sender);
$pp = $parts($mails[3]);
ok(array_keys($pp) === ['text/plain', 'image/tiff'] && strpos($pp['text/plain'], 'No PDF: a coding') !== false,
   'send: no PDF, the TIFF instead, said in the text');
ok(!FaxMail::send($fs, ['email' => 'a@example.com', 'name' => 'A'], '1700000000.9', $vars, $sender)[0],
   'send: no fax file, no mail');
ok(!FaxMail::send($fs, ['email' => '', 'name' => 'A'], '1700000000.5', $vars, $sender)[0],
   'send: no address, no mail');
ok(!FaxMail::send($fs, ['email' => 'a@example.com', 'name' => 'A'], '1700000000.5',
		  ['status' => 'FAILED', 'pages' => '0'], $sender)[0], 'send: a failed call with no pages, no mail');
same(count($mails), 4, 'send: nothing mailed for those');
ok(!FaxMail::send($fs, ['email' => 'a@example.com', 'name' => 'A'], '1700000000.5', $vars,
		  function () { return false; })[0], 'send: the mail system\'s refusal said');

/* the settings */
$set = $m->getSettings();
foreach (['fax_min_rate' => 1000, 'fax_paper' => 'legal', 'fax_attach' => 'zip',
	  'fax_email_from' => "a@b.co\nBcc: c@d.ef", 'fax_email_subject' => "a\nb"] as $k => $v)
	ok(isset(Config::validateSettings(array_merge($set, [$k => $v]))[$k]), "bad setting $k refused");
ok(isset(Config::validateSettings(array_merge($set, ['fax_min_rate' => 14400, 'fax_max_rate' => 9600]))['fax_min_rate']),
   'fax_min_rate not above fax_max_rate');
same(Config::validateSettings(array_merge($set, ['fax_min_rate' => 4800,
	'fax_email_from' => 'Fax Server <fax@example.com>'])), [], 'good fax-to-email settings');
ok(strpos(Config::render(array_merge($set, ['fax_min_rate' => 4800]), []), "fax_min_rate = 4800\n") !== false,
   'dmodem.conf: fax_min_rate');
$h = dmodem_get($m, ['view' => 'settings']);
ok(strpos($h, 'name="fax_email_subject"') !== false && strpos($h, '<textarea') !== false,
   'settings page: the fax-to-email section');

/* the recipients */
$sales = ['action' => 'save_recipient', 'original' => '', 'id' => 'Sales', 'name' => 'Sales <team>',
	  'email' => 'sales@example.com', 'user' => '', 'attach' => ''];
dmodem_post($m, $sales);
same($m->errors, [], 'recipient sales saved');
ok($m->getRecipient('sales') !== null, 'the id lowercased: sales');
foreach (['id' => ['id' => '_x'], 'id ' => ['id' => 'sales'], 'name' => ['name' => ''],
	  'email' => ['email' => 'nobody'], 'email ' => ['email' => ''],
	  'user' => ['email' => '', 'user' => 'bob'], 'attach' => ['attach' => 'zip']] as $field => $over) {
	dmodem_post($m, array_merge($sales, ['id' => 'other'], $over));
	ok(isset($m->errors[trim($field)]), 'bad recipient field ' . trim($field) . ' refused');
	ok($m->getRecipient('other') === null, 'a bad recipient is not kept (' . trim($field) . ')');
}
dmodem_post($m, array_merge($sales, ['original' => 'sales', 'id' => 'renamed', 'attach' => 'both']));
ok($m->getRecipient('renamed') === null && $m->getRecipient('sales')['attach'] === 'both',
   'a recipient edit keeps the id');
ok(isset(Config::validateProfile(array_merge(Config::defaultProfile(), ['name' => 'fax-sales']))['name']),
   'a profile cannot take a recipient\'s extension (fax-...)');
/* User Manager: a user's address */
$um = new FakeUserman();
$um->users = [['username' => 'bob', 'email' => 'bob@example.com'], ['username' => 'nomail', 'email' => '']];
FreePBX::$modules['Userman'] = $um;
same($m->usermanUsers(), ['bob' => 'bob@example.com'], 'User Manager: the users with an address');
dmodem_post($m, array_merge($sales, ['id' => 'bob', 'name' => 'Bob', 'email' => '', 'user' => 'bob']));
same($m->errors, [], 'a recipient of a User Manager user');
same($m->recipientAddress($m->getRecipient('bob'))['email'], 'bob@example.com', 'the user\'s address');
$h = dmodem_get($m, ['view' => 'recipient', 'recipient' => 'bob']);
ok(strpos($h, '<option value="bob" selected>bob (bob@example.com)</option>') !== false,
   'recipient form: the User Manager users');
unset(FreePBX::$modules['Userman']);
same($m->usermanUsers(), null, 'no User Manager');
same($m->recipientAddress($m->getRecipient('bob'))['email'], '', 'no User Manager: no address');
ok(!$m->faxToMail('bob', '1700000000.5', $vars, $sender)[0], 'faxToMail: no address, no mail');
ok(!$m->faxToMail('gone', '1700000000.5', $vars, $sender)[0], 'faxToMail: no such recipient');
$m->saveSettings(array_merge($m->getSettings(), ['fax_dir' => $faxdir]));
$r = $m->faxToMail('sales', '1700000000.5', $vars, $sender);
ok($r[0] && end($mails)['to'] === 'sales@example.com' &&
   array_keys($parts(end($mails))) === ['text/plain', 'application/pdf', 'image/tiff'],
   'faxToMail: the recipient\'s address and attachments');

/* the fax log */
same(FaxLog::uniqueidOk('1700000000.5'), true, 'FaxLog: a uniqueid');
same(FaxLog::uniqueidOk('1.2; rm'), false, 'FaxLog: not a uniqueid');
same(FaxLog::fileName('../etc/passwd.tif'), '', 'FaxLog: no path in a file name');
same(FaxLog::fileName('a-1.5.tif'), 'a-1.5.tif', 'FaxLog: a file name');
$n = count($mails);
$lines = [];
same(Cli::run($m, ['fax2mail', 'sales', '1700000000.5', 'SUCCESS', '1', '5550100', '+1 555 0100', 'ACME', '14400'],
	       ['send' => $sender], $say), 0, 'fwconsole dmodem fax2mail: logged and mailed');
same(count($mails), $n + 1, 'fax2mail: one mail');
$e = $m->getFax('1700000000.5');
ok($e !== null && $e['recipient'] === 'sales' && $e['pages'] === 1 && $e['rate'] === 14400 &&
   $e['cidname'] === 'ACME' && $e['file'] === basename($faxfile) && $e['mail_ok'],
   'the fax log: the entry of the fax');
same(Cli::run($m, ['fax2mail', '', '1700000000.8', 'FAILED', '0', '5550111', '', '', ''],
	       ['send' => $sender], $say), 0, 'fax2mail without a recipient: logged only');
same(count($mails), $n + 1, 'fax2mail without a recipient: no mail');
same(array_keys($m->getFaxes()), ['1700000000.8', '1700000000.5'], 'the fax log: newest first');
same($m->getFax('1700000000.8')['file'], '', 'the fax log: a failed fax without a file');
same(Cli::run($m, ['fax2mail', '', '1.2;x'], [], $say), 1, 'fax2mail: a bad uniqueid refused');
$lines = [];
same(Cli::run($m, ['faxes'], [], $say), 0, 'fwconsole dmodem faxes');
ok(count($lines) === 2 && strpos($lines[1], '1700000000.5') !== false && strpos($lines[1], 'sales') !== false,
   'fwconsole dmodem faxes: a line for each fax');
$d = $m->faxDocument($m->getFax('1700000000.5'), 'pdf');
ok($d !== null && $d[0] === 'application/pdf' && strpos($d[2], '%PDF-') === 0, 'faxDocument: the PDF');
$d = $m->faxDocument($m->getFax('1700000000.5'), 'tiff');
ok($d !== null && $d[0] === 'image/tiff' && $d[2] === file_get_contents($faxfile), 'faxDocument: the TIFF');
same($m->faxDocument($m->getFax('1700000000.8'), 'pdf'), null, 'faxDocument: none without a file');
$setting = [];
ok($m->ajaxRequest('faxpdf', $setting) && $setting['authenticate'] === true, 'Ajax: faxpdf, signed in only');
ok(!$m->ajaxRequest('other', $setting), 'Ajax: no other command');
$_REQUEST = ['command' => 'faxtiff', 'uniqueid' => '1700000000.5'];
ob_start();
ok($m->ajaxCustomHandler(), 'Ajax: the TIFF sent');
same(ob_get_clean(), file_get_contents($faxfile), 'Ajax: the TIFF\'s bytes');
$_REQUEST = ['command' => 'faxpdf', 'uniqueid' => '9.9'];
ob_start();
$m->ajaxCustomHandler();
ok(strpos(ob_get_clean(), 'No such fax') !== false, 'Ajax: no such fax');
$h = dmodem_get($m, ['view' => 'faxes']);
ok(strpos($h, 'command=faxpdf&amp;uniqueid=1700000000.5') !== false && strpos($h, 'Sales &lt;team&gt;') !== false &&
   strpos($h, 'No file') !== false, 'faxes page: the list, the links, the recipient\'s name');
ok(strpos(dmodem_get($m, []), 'view=faxes') !== false, 'the page: a Faxes tab');
/* fax_keep_days */
ok(isset(Config::validateSettings(array_merge($m->getSettings(), ['fax_keep_days' => -1]))['fax_keep_days']),
   'fax_keep_days: not below 0');
$m->saveSettings(array_merge($m->getSettings(), ['fax_keep_days' => 30]));
same($m->purgeFaxes(time() + 10 * 86400), 0, 'purge: nothing under 30 days');
same($m->purgeFaxes(time() + 31 * 86400), 2, 'purge: the faxes over 30 days');
ok(!file_exists($faxfile) && $m->getFaxes() === [], 'purge: the entries and the file gone');
file_put_contents($faxfile, mh_white_tiff(1));
$m->saveSettings(array_merge($m->getSettings(), ['fax_keep_days' => 0]));
$m->faxReceived('', '1700000000.5', $vars);
same($m->purgeFaxes(time() + 3650 * 86400), 0, 'purge: 0 days keeps all');
dmodem_post($m, ['action' => 'delete_fax', 'uniqueid' => '1700000000.5']);
ok($m->message[0] === 'success' && $m->getFax('1700000000.5') === null && !file_exists($faxfile),
   'delete_fax: the entry and its file gone');
dmodem_post($m, ['action' => 'delete_fax', 'uniqueid' => '1700000000.5']);
same($m->message[0], 'danger', 'delete_fax: no such fax');
$old = [];
for ($i = 0; $i < FaxLog::MAX_ENTRIES + 3; $i++)
	$old["1.$i"] = ['time' => $i];
same(count(FaxLog::expired($old, 0, 0)), 3, 'FaxLog: the oldest above MAX_ENTRIES go');
same(FaxLog::expired($old, 0, 0)[0], '1.2', 'FaxLog: the oldest go first');

/* junk fax control */
same(FaxJunk::parse("5550100 # a sender\n\n 1900* \nAnonymous"), ['5550100', '1900*', 'anonymous'],
     'FaxJunk: the lines, comments and spaces removed');
same(FaxJunk::listError("+1 (555) 010-0\n1900*\nanonymous"), '', 'FaxJunk: a good list');
foreach (["555x", "*", "19*00", "anon", str_repeat('1', 41)] as $bad)
	ok(FaxJunk::listError($bad) !== '', "FaxJunk: a bad line refused ($bad)");
ok(FaxJunk::listError(str_repeat("1\n", FaxJunk::MAX_LINES + 1)) !== '', 'FaxJunk: not too many lines');
ok(FaxJunk::matches('+1 555 0100', '15550100') && !FaxJunk::matches('15550101', '15550100'),
   'FaxJunk: a number matches its digits only');
ok(FaxJunk::matches('19005551234', '1900*') && !FaxJunk::matches('18005551234', '1900*'),
   'FaxJunk: a prefix');
ok(FaxJunk::matches('', 'anonymous') && FaxJunk::matches('Restricted', 'anonymous') &&
   !FaxJunk::matches('5550100', 'anonymous') && !FaxJunk::matches('', '1900*'),
   'FaxJunk: anonymous, only for a call without a number');
$js = ['fax_block' => "1900*\nanonymous", 'fax_allow' => '19005550000', 'fax_allow_only' => false];
same(FaxJunk::blocked('19001234567', $js), 'On the block list', 'FaxJunk: blocked');
same(FaxJunk::blocked('19005550000', $js), '', 'FaxJunk: the allow list comes first');
same(FaxJunk::blocked('5550100', $js), '', 'FaxJunk: not blocked');
same(FaxJunk::blocked('5550100', ['fax_allow_only' => true] + $js), 'Not on the allow list',
     'FaxJunk: only the allow list');
same(FaxJunk::add("1900*\n", '+1 555 0100'), "1900*\n15550100", 'FaxJunk: a caller ID added');
same(FaxJunk::add("15550100", '15550100'), '15550100', 'FaxJunk: not added twice');
same(FaxJunk::add('', 'anonymous'), '', 'FaxJunk: no number, nothing added');
$st = $m->getSettings();
ok(isset(Config::validateSettings(array_merge($st, ['fax_block' => 'junk']))['fax_block']),
   'settings: a bad block list refused');
ok(isset(Config::validateSettings(array_merge($st, ['fax_allow_only' => true]))['fax_allow']),
   'settings: only the allow list needs a number on it');
same(Config::validateSettings(array_merge($st, ['fax_block' => '1900*', 'fax_allow' => '5550100',
	'fax_allow_only' => true])), [], 'settings: good lists');
$m->saveSettings(array_merge($st, ['fax_block' => "1900*\n"]));
$n = count($mails);
same(Cli::run($m, ['fax2mail', 'sales', '1700000000.9', 'SUCCESS', '1', '19005550123', '', 'JUNK', '9600'],
	       ['send' => $sender], $say), 0, 'fax2mail: a blocked fax');
$e = $m->getFax('1700000000.9');
ok($e['blocked'] && FaxLog::status($e) === 'BLOCKED' && $e['mail'] === 'On the block list' &&
   count($mails) === $n, 'a blocked fax: logged as BLOCKED, not mailed');
$m->faxReceived('', '1700000000.10', ['status' => 'SUCCESS', 'cid' => '5550123'], $sender);
ok(!$m->getFax('1700000000.10')['blocked'], 'a fax from a different caller ID: not blocked');
$h = dmodem_get($m, ['view' => 'faxes']);
ok(strpos($h, 'BLOCKED') !== false && substr_count($h, 'value="block_caller"') === 1,
   'faxes page: BLOCKED, and a block button for the fax that is not blocked');
dmodem_post($m, ['action' => 'block_caller', 'uniqueid' => '1700000000.10']);
ok($m->message[0] === 'success' && $m->getSettings()['fax_block'] === "1900*\n5550123",
   'block_caller: the caller ID on the block list');
dmodem_post($m, ['action' => 'block_caller', 'uniqueid' => '9.9']);
same($m->message[0], 'danger', 'block_caller: no such fax');
$m->saveSettings(['fax_allow' => '5550123']);
dmodem_post($m, ['action' => 'block_caller', 'uniqueid' => '1700000000.10']);
same($m->message[0], 'danger', 'block_caller: not a caller ID on the allow list');
$m->faxReceived('', '1700000000.11', ['status' => 'SUCCESS'], $sender);
dmodem_post($m, ['action' => 'block_caller', 'uniqueid' => '1700000000.11']);
same($m->message[0], 'danger', 'block_caller: not without a caller ID number');
ok(!FaxLog::normalize(['uniqueid' => '1.1', 'direction' => 'out', 'blocked' => true])['blocked'],
   'FaxLog: a sent fax is never blocked');
foreach (['1700000000.9', '1700000000.10', '1700000000.11'] as $u)
	$m->deleteFax($u);
$m->saveSettings(array_merge($st, ['fax_block' => '', 'fax_allow' => '']));

$m->saveSettings(array_merge($m->getSettings(), ['fax_dir' => '/var/spool/asterisk/fax']));
array_map('unlink', glob("$faxdir/*"));
rmdir($faxdir);
$lines = [];
same(Cli::run($m, ['fax2mail', 'sales'], [], $say), 2, 'fwconsole dmodem fax2mail: usage');
same(Cli::run($m, ['fax2mail', 'gone', '1.2', 'SUCCESS', '1', '', '', ''], [], $say), 1,
     'fwconsole dmodem fax2mail: no such recipient');
ok(strpos(end($lines), 'No fax recipient gone') !== false, 'fwconsole dmodem fax2mail: says why');
dmodem_post($m, ['action' => 'delete_recipient', 'recipient' => 'bob']);
ok($m->getRecipient('bob') === null, 'recipient bob deleted');

/* the recipient's destination and dialplan */
$d = dmodem_destinations();
ok(in_array('app-dmodem,fax-sales,1', array_column($d, 'destination'), true), 'a destination for the recipient');
same(end($d)['description'], 'Fax to Sales <team>', 'the recipient\'s destination named');
same(dmodem_getdestinfo('app-dmodem,fax-sales,1')['edit_url'],
     'config.php?display=dmodem&view=recipient&recipient=sales', 'getdestinfo: the recipient\'s page');
same(dmodem_getdestinfo('app-dmodem,fax-gone,1'), [], 'getdestinfo of a gone recipient: empty');
$dp = dmodem_dialplan($m);
ok(strpos($dp, "exten => fax-sales,1,NoOp(D-Modem fax to sales)\n") !== false, 'the recipient\'s extension');
ok(strpos($dp, "exten => fax-sales,n,Set(CHANNEL(hangup_handler_push)=app-dmodem-fax2mail,s,1(sales))\n") !== false,
   'the recipient\'s extension pushes the mail handler');
ok(strpos($dp, "exten => fax-sales,n,DModemReceiveFax(/var/spool/asterisk/fax/") !== false,
   'the recipient\'s extension takes the fax');
ok(strpos($dp, "[app-dmodem-fax2mail]\n") !== false &&
   strpos($dp, "exten => s,1,ExecIf(\$[\"\${DMODEMFAXSTATUS}\" = \"\"]?Return())\n") !== false &&
   strpos($dp, "exten => s,n,System(/usr/sbin/fwconsole dmodem fax2mail '\${FILTER(a-z0-9_,\${ARG1})}' '\${FILTER(0-9.,\${UNIQUEID})}'") !== false &&
   strpos($dp, "'\${FILTER(0-9,\${DMODEMFAXRATE})}')\n") !== false,
   'the mail handler: nothing without a fax, then fwconsole dmodem fax2mail, its values filtered');
ok(strpos($dp, "exten => s,n,Return()\n") !== false, 'the mail handler returns');
$cfg = new FakeConfig();
$cfg->values['AMPSBIN'] = '/opt/freepbx/sbin';
FreePBX::$modules['Config'] = $cfg;
ok(strpos(dmodem_dialplan($m), 'System(/opt/freepbx/sbin/fwconsole dmodem fax2mail') !== false,
   'the mail handler: fwconsole from AMPSBIN');
$cfg->values['AMPSBIN'] = '/bin; rm -rf /';
ok(strpos(dmodem_dialplan($m), 'System(/usr/sbin/fwconsole dmodem') !== false,
   'the mail handler: a strange AMPSBIN not taken');
unset(FreePBX::$modules['Config']);
$GLOBALS['dest_usage']['app-dmodem,fax-sales,1'] = ['core' => ['inbound route 5551001']];
dmodem_post($m, ['action' => 'delete_recipient', 'recipient' => 'sales']);
ok($m->getRecipient('sales') !== null && $m->message[0] === 'danger', 'a recipient in use is not deleted');
$GLOBALS['dest_usage'] = [];

/* the pages */
$h = dmodem_get($m, ['view' => 'recipients']);
ok(strpos($h, 'Sales &lt;team&gt;') !== false && strpos($h, 'app-dmodem,fax-sales,1') !== false,
   'recipients page: the name escaped, the destination');
$h = dmodem_get($m, ['view' => 'recipient', 'recipient' => 'sales']);
ok(strpos($h, 'value="sales@example.com"') !== false && strpos($h, 'readonly') !== false &&
   strpos($h, 'action=delete_recipient&amp;recipient=sales') !== false, 'recipient form: its values, the delete URL');
same(array_keys($m->getActionBar(['display' => 'dmodem', 'view' => 'recipient', 'recipient' => 'sales'])),
     ['reset', 'submit', 'delete'], 'action bar: an existing recipient');
same(array_keys($m->getActionBar(['display' => 'dmodem', 'view' => 'recipient'])),
     ['reset', 'submit'], 'action bar: a new recipient');
$h = dmodem_get($m, ['view' => 'dialplan']);
ok(strpos($h, '[app-dmodem-fax2mail]') !== false, 'generated files page: the mail handler');

/* the check page: what the fax machine can do */
$am->out['dmodem fax show capabilities'] = "Technology:   DModem (libdmodem T.30)\nECM:          yes\n";
$ck = $m->check($m->getSettings());
ok(strpos($ck['fax'], 'DModem (libdmodem T.30)') !== false, 'check: app_dmodem\'s fax capabilities');
ok(strpos($m->faxCapabilities(array_merge($m->getSettings(), ['engine' => 'ari', 'ari_t38' => true]), true),
	  'T.38 (versions 0 to 3, on the SIP side)') !== false, 'check: dmodem-ari\'s fax capabilities');
same($m->faxCapabilities($m->getSettings(), false), '', 'check: no engine, no fax capabilities');
$h = dmodem_get($m, ['view' => 'check']);
ok(strpos($h, 'id="dmodem-fax-capabilities"') !== false, 'check page: the fax capabilities');

/* the ARI engine: the recipient through Stasis() */
$dpa = Dialplan::render(Dialplan::build(array_merge($m->getSettings(), ['engine' => 'ari']), [],
					$m->getRecipients()));
ok(strpos($dpa, " same => n,Stasis(dmodem,receivefax,/var/spool/asterisk/fax/") !== false,
   'ari: the recipient\'s fax through Stasis(<app>,receivefax,<file>)');

/* --- fax detection on inbound routes (Core's display=did page) --- */

/* the inbound route form posted to the module after Core saved it */
function dmodem_did_post($m, array $r)
{
	$_REQUEST = $_POST = $r + ['display' => 'did'];
	$m->doConfigPageInit('did');
}

/* Core's routes in ext-did-000x (a part of what core_get_config adds),
   then the module's dialplan hook */
function dmodem_did_dialplan($m, array $routes)
{
	$ext = new extensions();
	foreach ($routes as $r) {
		list($ctx, $exten) = $r;
		$ext->add($ctx, $exten, '', new ext_dmodem_app('Set', '__FROM_DID=${EXTEN}'));
		$ext->add($ctx, $exten, 'did', new ext_dmodem_app('Set', 'CDR(did)=${FROM_DID}'));
		$ext->add($ctx, $exten, 'did-cid-hook', new ext_dmodem_app('NoOp', 'CallerID Entry Point'));
		$ext->add($ctx, $exten, 'dest-ext', new ext_dmodem_app('Goto', 'app-dmodem,bbs,1'));
	}
	$m->doDialplanHook($ext, 'asterisk', 500);
	return $ext->generateConf();
}

same(Dialplan::didExten('5551001', ''), '5551001', 'didExten: a DID');
same(Dialplan::didExten('5551001', '5550100'), '5551001/5550100', 'didExten: a DID and a caller ID');
same(Dialplan::didExten('', '5550100'), 's/5550100', 'didExten: a caller ID alone');
same(Dialplan::didExten('', ''), 's', 'didExten: any DID');
same(Config::didKey(' <5551001> ', ''), '5551001/', 'didKey: as Core trims a route');
same(Config::didKey("1\n2", ''), null, 'didKey: no line breaks');
same(FreePBX\modules\Dmodem::myConfigPageInits(), ['did'], 'the module hooks the inbound route page');

$route = ['action' => 'addIncoming', 'extension' => '5551001', 'cidnum' => '',
	  'dmodem_fax_form' => '1', 'dmodem_fax_detect' => 'yes', 'dmodem_fax_listen' => 'both',
	  'dmodem_fax_wait' => '6', 'dmodem_fax_ring' => 'yes',
	  'gotodmodemfax' => 'dest', 'destdmodemfax' => 'app-dmodem,fax-sales,1'];
$GLOBALS['needreload'] = 0;
dmodem_did_post($m, $route);
same($m->getDidFax('5551001/'), ['detect' => true, 'listen' => 'both', 'wait' => 6, 'ring' => true,
				'dest' => 'app-dmodem,fax-sales,1'], 'a route\'s fax detection kept');
ok($GLOBALS['needreload'] > 0, 'a route\'s fax detection asks for Apply Config');
dmodem_did_post($m, array_merge($route, ['dmodem_fax_wait' => '99']));
same($m->getDidFax('5551001/')['wait'], 6, 'a bad time: the fax detection stays as it was');
dmodem_did_post($m, array_merge($route, ['destdmodemfax' => '']));
same($m->getDidFax('5551001/')['dest'], 'app-dmodem,fax-sales,1', 'no destination: the fax detection stays');
$noform = $route;
unset($noform['dmodem_fax_form']);
dmodem_did_post($m, array_merge($noform, ['dmodem_fax_detect' => '']));
ok($m->getDidFax('5551001/')['detect'], 'a post without the tab (another caller) changes nothing');
/* a route renamed: its fax detection goes with it */
dmodem_did_post($m, array_merge($route, ['action' => 'edtIncoming', 'old_extension' => '5551001',
	'old_cidnum' => '', 'extension' => '5551002', 'dmodem_fax_listen' => 'cng',
	'dmodem_fax_ring' => '']));
ok(!$m->getDidFax('5551001/')['detect'] && $m->getDidFax('5551002/')['detect'],
   'a renamed route: the fax detection moved');
dmodem_did_post($m, array_merge($route, ['cidnum' => '5550100', 'extension' => '']));
same(array_keys($m->getDidFaxes()), ['/5550100', '5551002/'], 'a route of a caller ID alone');

$dp = dmodem_did_dialplan($m, [['ext-did-0002', '5551002'], ['ext-did-0001', 's/5550100'],
			       ['ext-did-0002', '5559999']]);
ok(strpos($dp, "exten => 5551002,n(did),Set(CDR(did)=\${FROM_DID})\n" .
	   "exten => 5551002,n,DModemFaxDetect(6,xc)\n" .
	   "exten => 5551002,n,GotoIf(\$[\"\${DMODEMFAXDETECTED}\" = \"CNG\" | \"\${DMODEMFAXDETECTED}\" = \"T38\"]?app-dmodem,fax-sales,1)\n" .
	   "exten => 5551002,n(did-cid-hook),NoOp(CallerID Entry Point)\n") !== false,
   'the route listens (CNG only, no ring) before its caller ID step, and a fax goes to its destination');
ok(strpos($dp, "exten => s/5550100,n,DModemFaxDetect(6,xr)\n") !== false,
   'the caller ID route listens (ring, CNG and T.38)');
ok(strpos($dp, "exten => 5559999,n,DModemFaxDetect") === false, 'a route without it: no fax detection');
ok(strpos($dp, 'ext-did-0001') !== false && substr_count($dp, 'DModemFaxDetect') === 2,
   'only the routes that are there (none added)');
$m->saveSettings(array_merge($m->getSettings(), ['engine' => 'ari', 'ari_app' => 'dm']));
$dp = dmodem_did_dialplan($m, [['ext-did-0002', '5551002']]);
ok(strpos($dp, "exten => 5551002,n,Stasis(dm,faxdetect,6,xc)\n") !== false,
   'ari: Stasis(<app>,faxdetect,<time>,<options>)');
$m->saveSettings(array_merge($m->getSettings(), ['engine' => 'app', 'ari_app' => 'dmodem']));

/* the tab on the route's page */
$_REQUEST = ['display' => 'did', 'view' => 'form', 'extdisplay' => '5551002/'];
$tab = $m->didFaxTab('did');
ok(count($tab) === 1 && $tab[0]['rawname'] === 'dmodemfax' && $tab[0]['title'] === 'D-Modem fax',
   'the inbound route page: a D-Modem fax tab');
ok(strpos($tab[0]['content'], 'id="dmodem_fax_detect" name="dmodem_fax_detect" value="yes" checked') !== false &&
   strpos($tab[0]['content'], '<option value="cng" selected>') !== false &&
   strpos($tab[0]['content'], 'value="app-dmodem,fax-sales,1"') !== false,
   'the tab: the route\'s fax detection');
$_REQUEST = ['display' => 'did', 'view' => 'form'];
ok(strpos($m->didFaxTab('did')[0]['content'], 'name="dmodem_fax_detect" value="yes">') !== false,
   'the tab of a new route: off');
same($m->didFaxTab('extensions'), [], 'no tab on other pages');

/* the destinations it uses */
same(array_column(dmodem_check_destinations(['app-dmodem,fax-sales,1']), 'description'),
     ['Inbound route /5550100: D-Modem fax', 'Inbound route 5551002/: D-Modem fax'],
     'check_destinations: the routes that send faxes there');
same(dmodem_check_destinations(['app-dmodem,bbs,1']), [], 'check_destinations: none for another');
same(count(dmodem_check_destinations(true)), 2, 'check_destinations: all');

/* a route deleted, and one turned off */
dmodem_did_post($m, ['action' => 'delIncoming', 'extdisplay' => '/5550100']);
same(array_keys($m->getDidFaxes()), ['5551002/'], 'a deleted route: its fax detection gone');
dmodem_did_post($m, array_merge($route, ['extension' => '5551003', 'dmodem_fax_detect' => '']));
same(array_keys($m->getDidFaxes()), ['5551002/'], 'off: none kept');

/* --- export, import, backup and restore --- */

$exp = $m->exportData();
same(array_keys($exp), ['format', 'settings', 'profiles', 'recipients', 'didfax'], 'export: its parts');
same(array_keys($exp['didfax']), ['5551002/'], 'export: the fax detection of the routes');
same(array_keys($exp['recipients']), ['sales'], 'export: the fax recipients');
same($exp['settings']['fax_ecm'], false, 'export: settings as they are');
$file = sys_get_temp_dir() . '/dmodem-unit-' . getmypid() . '.json';
same(Cli::run($m, ['export', $file], [], $say), 0, 'fwconsole dmodem export <file>');
/* (Windows' files have no owner/group/other modes: FreePBX runs on POSIX) */
if (PHP_OS_FAMILY !== 'Windows')
	same(fileperms($file) & 0777, 0600, 'the export readable by its owner alone (the ARI password)');

/* into a fresh module, as on another system */
$saved = unserialize(serialize(FreePBX_Helpers::$kv));
$m3 = dmodem_new();
same(Cli::run($m3, ['import', $file], [], $say), 0, 'fwconsole dmodem import');
same($m3->exportData(), $exp, 'import: the settings and profiles as exported');
unlink($file);

/* a bad profile is left out, the rest goes in; replace drops the others */
dmodem_post($m3, array_merge($bbs, ['name' => 'extra', 'original' => '']));
$bad = $exp;
$bad['profiles']['evil'] = array_merge($exp['profiles']['bbs'], ['mode' => 'exec', 'command' => '']);
$bad['profiles']['general'] = $exp['profiles']['bbs'];
$r = $m3->importData($bad);
ok(!$r['ok'] && isset($r['skipped']['evil']['command']) && isset($r['skipped']['general']['name']),
   'import: bad profiles left out, and said why');
same(array_keys($m3->getProfiles()), ['bbs'], 'import: the profiles of the file alone');
dmodem_post($m3, array_merge($bbs, ['name' => 'extra', 'original' => '']));
$m3->importData($exp, false);
same(array_keys($m3->getProfiles()), ['bbs', 'extra'], 'import --keep: the others stay');
ok(!$m3->importData(['profiles' => []])['ok'], 'import: not an export');
$r = $m3->importData(array_merge($exp, ['settings' => array_merge($exp['settings'], ['fax_dir' => 'x'])]));
ok(isset($r['settings']['fax_dir']) && $m3->getSettings()['fax_dir'] === '/var/spool/asterisk/fax',
   'import: bad settings not kept');

/* FreePBX's Backup & Restore: the module's own classes */
require_once __DIR__ . '/../dmodem/Backup.php';
require_once __DIR__ . '/../dmodem/Restore.php';
FreePBX::$modules['Dmodem'] = $m;
$b = new \FreePBX\modules\Dmodem\Backup($m->FreePBX, '17.0', null, 'tx1',
	['module' => 'dmodem', 'version' => '16.0.2'], false);
$b->runBackup('id1', 'tx1');
same($b->getConfigs(), $exp, 'backup: the export in the backup\'s configs');
/* the restore after the backup module's reset (an empty store) */
$m4 = dmodem_new();
FreePBX::$modules['Dmodem'] = $m4;
$rs = new \FreePBX\modules\Dmodem\Restore($m4->FreePBX, '17.0', null, 'tx2',
	['module' => 'dmodem', 'configs' => json_decode(json_encode($b->getConfigs()), true)],
	'/tmp', false);
$rs->runRestore();
same($m4->exportData(), $exp, 'restore: the settings and profiles back');
ok(strpos(json_encode($rs->logged), '1 profiles and 1 fax recipients restored') !== false, 'restore: logged');
FreePBX_Helpers::$kv = $saved;
FreePBX::$modules['Dmodem'] = $m;

/* --- sending faxes --- */

$sdir = sys_get_temp_dir() . '/dmodem-send-' . getmypid();
@mkdir($sdir);
$m->saveSettings(array_merge($m->getSettings(), ['fax_dir' => $sdir, 'fax_retries' => 1,
	'fax_retry_delay' => 5, 'fax_notify' => 'failure', 'fax_notify_email' => 'ops@example.com',
	'fax_cover_name' => 'ACME (Fax) \\ Co', 'fax_cover_address' => "1 Main St\nSpringfield"]));
/* the cover page */
$cv = FaxCover::pdf(['name' => 'ACME (Fax)', 'to' => 'Bob', 'number' => '5550100',
		     'message' => str_repeat('word ', 100), 'pages' => 3, 'footer' => 'Private'], 'a4');
ok(strpos($cv, '%PDF-1.4') === 0 && strpos($cv, '/MediaBox [0 0 595 842]') !== false,
   'FaxCover: a PDF of A4');
ok(strpos($cv, '(ACME \\(Fax\\))') !== false && strpos($cv, '(3, with this page)') !== false &&
   strpos($cv, '(Private)') !== false, 'FaxCover: the text escaped, the pages, the footer');
same(count(FaxCover::wrap(str_repeat("x\n", 50))), FaxCover::MAX_LINES, 'FaxCover: the lines cut');
same(FaxCover::str("caf\xc3\xa9"), "(caf\xe9)", 'FaxCover: UTF-8 to WinAnsi');
if (is_executable('/usr/local/bin/pdftoppm') || is_executable('/usr/bin/pdftoppm')) {
	file_put_contents("$sdir/cover.pdf", $cv);
	exec('pdftoppm -r 20 -gray ' . escapeshellarg("$sdir/cover.pdf") . ' ' .
	     escapeshellarg("$sdir/cv") . ' 2>&1', $o, $rc);
	ok($rc === 0 && glob("$sdir/cv*.pgm"), 'FaxCover: Poppler reads the cover page');
	array_map('unlink', glob("$sdir/cv*.pgm"));
	unlink("$sdir/cover.pdf");
}
/* the job's checks */
$st = $m->getSettings();
$jb = FaxSend::normalizeJob(['number' => '5550100', 'retries' => '2', 'cover' => 'yes'], $st);
ok($jb['cover'] === true && $jb['retries'] === 2 && $jb['email'] === 'ops@example.com',
   'FaxSend: a job over the settings\' defaults');
same(FaxSend::validateJob($jb, 0), [], 'FaxSend: a cover page alone is a fax');
foreach (['number' => ['number' => '555;0100'], 'docs' => ['cover' => ''],
	  'retries' => ['retries' => '4'], 'notify' => ['notify' => 'sometimes'],
	  'email' => ['email' => 'nobody'], 'to' => ['to' => "a\nb"]] as $k => $over)
	ok(isset(FaxSend::validateJob(FaxSend::normalizeJob(array_merge(
		['number' => '5550100', 'cover' => 'yes'], $over), $st), 0)[$k]), "FaxSend: bad $k refused");
same(FaxSend::kind('%PDF-1.4'), 'pdf', 'FaxSend: a PDF');
same(FaxSend::kind("II*\0"), 'tiff', 'FaxSend: a TIFF');
same(FaxSend::kind('<html>'), null, 'FaxSend: not a document');
same(FaxSend::exec(['/nonexistent/gs', '-v'])[0], 127, 'send: a Ghostscript that is not there');
$cmd = FaxSend::gsCommand('/usr/bin/gs', ['/a.pdf', '/b.pdf'], '/o.tif', 'a4');
ok(in_array('-sDEVICE=tiffg4', $cmd, true) && in_array('-g1728x2292', $cmd, true) &&
   in_array('-dSAFER', $cmd, true) && array_slice($cmd, -2) === ['/a.pdf', '/b.pdf'],
   'FaxSend: the Ghostscript command (no shell)');
same(Config::validateSettings(array_merge($st, ['fax_retries' => 5]))['fax_retries'], 'From 0 to 3',
     'settings: fax_retries');
ok(isset(Config::validateSettings(array_merge($st, ['fax_ghostscript' => 'gs; rm']))['fax_ghostscript']),
   'settings: a strange Ghostscript path refused');
/* a stand-in Ghostscript: a fax file of one page */
$gsRuns = [];
$fakeGs = function (array $c) use (&$gsRuns) {
	$gsRuns[] = $c;
	foreach ($c as $a)
		if (strpos($a, '-sOutputFile=') === 0)
			$out = substr($a, 13);
	file_put_contents($out, mh_white_tiff(3));
	return [0, ''];
};
$tif = mh_white_tiff(2);
$r = $m->queueFax(['number' => '5550100', 'cover' => 'yes', 'to' => 'Bob', 'message' => 'Hi',
		   'notify' => 'always'], [['a.pdf', "%PDF-1.4\n"], ['b.tif', $tif]], 1800000000, $fakeGs);
ok($r['ok'] && FaxSend::idOk($r['id']), 'queueFax: queued');
$jid = $r['id'];
same(count($gsRuns), 2, 'queueFax: Ghostscript for the pages, then with the cover page');
same(count(preg_grep('/\.pdf$/', $gsRuns[1])), 3,
     'queueFax: the cover page and two documents');
same($m->getFaxJob($jid)['pages'], 1, 'queueFax: the pages counted');
ok(is_file("$sdir/out-$jid.tif") && !is_dir("$sdir/out-$jid.tif.d"), 'queueFax: the fax file, no temporary files');
$r = $m->queueFax(['number' => '5550100'], [['x.html', '<html>']], 1800000000, $fakeGs);
ok(!$r['ok'] && strpos($r['errors']['docs'], 'not a PDF or TIFF') !== false, 'queueFax: not a document');
$r = $m->queueFax(['number' => '5550100'], [['x.pdf', '%PDF-']], 1800000000,
		  function ($c) { return [1, 'Error: /syntaxerror']; });
ok(!$r['ok'] && strpos($r['message'], '/syntaxerror') !== false, 'queueFax: Ghostscript\'s error');
same(count($m->getFaxJobs()), 1, 'queueFax: only the good job queued');
if (is_executable('/usr/bin/gs')) {
	$r = $m->queueFax(['number' => '5550101', 'cover' => 'yes', 'message' => 'Real'],
			  [['b.tif', mh_white_tiff(4)]], 1800000001);
	ok($r['ok'] && $m->getFaxJob($r['id'])['pages'] === 2, 'queueFax: Ghostscript\'s own pages (cover + 1)');
	if (!$r['ok'] || $m->getFaxJob($r['id'])['pages'] !== 2)
		echo "  ($r[message])\n";
	$m->cancelFax($r['id']);
}
/* the queue: a try */
$am = $m->FreePBX->astman;
$am->sent = [];
$m->runFaxQueue(1800000010);
same($am->sent[0][0], 'Originate', 'runFaxQueue: an Originate');
same(array_intersect_key($am->sent[0][1], array_flip(['Channel', 'Context', 'Exten', 'Variable'])),
     ['Channel' => 'Local/5550100@from-internal', 'Context' => 'app-dmodem-faxout', 'Exten' => 's',
      'Variable' => 'DMODEMFAXJOB=' . $jid], 'runFaxQueue: the call to the fax context');
ok($m->getFaxJob($jid)['state'] === 'sending' && $m->getFaxJob($jid)['attempts'] === 1,
   'runFaxQueue: the job on a call');
$m->runFaxQueue(1800000020);
same(count($am->sent), 1, 'runFaxQueue: no second call while on a call');
/* a failed try: the next one later */
$lines = [];
same(Cli::run($m, ['faxsent', $jid, '1800000030.4', 'FAILED', '0', '0', 'T30_NO_ANSWER', ''], [], $say), 0,
     'fwconsole dmodem faxsent: a failed try');
ok($m->getFaxJob($jid)['state'] === 'retry' && $m->getFaxJob($jid)['next'] >= time() + 299,
   'faxsent: the next try in 5 minutes');
ok(strpos(end($lines), 'try 2 of 2') !== false, 'faxsent: says the next try');
$m->runFaxQueue(time() + 10);
same(count($am->sent), 1, 'runFaxQueue: not before the next try');
$lines = [];
same(Cli::run($m, ['faxjobs'], [], $say), 0, 'fwconsole dmodem faxjobs');
ok(strpos($lines[0], $jid) !== false && strpos($lines[0], 'retry') !== false, 'faxjobs: the job');
$m->runFaxQueue(time() + 400);
same(count($am->sent), 2, 'runFaxQueue: the second try');
/* the last try: into the log, the result by email */
$n = count($mails);
same(Cli::run($m, ['faxsent', $jid, '1800000500.7', 'SUCCESS', '3', '9600', 'OK', '+1 555 0100'],
	       ['send' => $sender], $say), 0, 'faxsent: sent');
same($m->getFaxJob($jid), null, 'faxsent: the job out of the queue');
$e = $m->getFax('1800000500.7');
ok($e !== null && $e['direction'] === 'out' && $e['number'] === '5550100' && $e['pages'] === 3 &&
   $e['attempts'] === 2 && $e['file'] === "out-$jid.tif" && $e['mail_ok'],
   'faxsent: the sent fax in the fax log');
same(count($mails), $n + 1, 'faxsent: the result mailed (notify always)');
ok(end($mails)['to'] === 'ops@example.com' && end($mails)['subject'] === 'Fax to 5550100 sent' &&
   isset($parts(end($mails))['application/pdf']), 'the result: to the address, its subject, the PDF');
$h = dmodem_get($m, ['view' => 'faxes']);
ok(strpos($h, '<td>Sent</td>') !== false && strpos($h, '5550100') !== false, 'faxes page: a sent fax');
/* no answer, no tries left: failed; refused by Asterisk */
$r = $m->queueFax(['number' => '5550102', 'cover' => 'yes', 'retries' => '0'], [], 1800000000, $fakeGs);
$m->runFaxQueue(1800000100);
$n = count($mails);
$m->faxSent($r['id'], '1800000600.1', 'NOANSWER', 0, 0, 'REASON_3', '', $sender);
ok($m->getFaxJob($r['id']) === null && $m->getFax('1800000600.1')['status'] === 'NOANSWER',
   'faxSent: no answer, no tries left: in the log');
same(count($mails), $n + 1, 'faxSent: the failure mailed (notify failure)');
$r = $m->queueFax(['number' => '5550103', 'cover' => 'yes', 'retries' => '0', 'notify' => 'never'],
		  [], 1800000000, $fakeGs);
$am->reply = ['Response' => 'Error', 'Message' => 'Extension does not exist.'];
$out = $m->runFaxQueue(1800000100);
$am->reply = ['Response' => 'Success', 'Message' => 'Originate successfully queued'];
ok($m->getFaxJob($r['id']) === null && strpos($out[0], 'Extension does not exist') !== false,
   'runFaxQueue: refused by Asterisk: a failed try');
/* a try that never reports */
$r = $m->queueFax(['number' => '5550104', 'cover' => 'yes', 'retries' => '0', 'notify' => 'never'],
		  [], 1800000000, $fakeGs);
$m->runFaxQueue(1800000000);
$m->runFaxQueue(1800000000 + 60 + FaxSend::MAX_TRY + 1);
ok($m->getFaxJob($r['id']) === null, 'runFaxQueue: a try without a result is a failure');
/* cancel */
$r = $m->queueFax(['number' => '5550105', 'cover' => 'yes'], [], 1900000000, $fakeGs);
dmodem_post($m, ['action' => 'cancel_fax', 'job' => $r['id']]);
ok($m->message[0] === 'success' && $m->getFaxJob($r['id']) === null, 'cancel_fax: out of the queue');
same(Cli::run($m, ['faxcancel', 'j1'], [], $say), 1, 'faxcancel: no such job');
same(Cli::run($m, ['faxsent', 'j1', '1.1', 'SUCCESS'], [], $say), 1, 'faxsent: no such job');
/* fwconsole dmodem sendfax */
file_put_contents("$sdir/in.tif", $tif);
$am->sent = [];
$lines = [];
same(Cli::run($m, ['sendfax', '5550106', "$sdir/in.tif"], ['run' => $fakeGs, 'retries' => '0',
	'notify' => 'never'], $say), 0, 'fwconsole dmodem sendfax');
ok(strpos($lines[0], 'queued (1 pages)') !== false && count($am->sent) === 1, 'sendfax: queued and called');
same(Cli::run($m, ['sendfax', '5550106', "$sdir/none.pdf"], [], $say), 1, 'sendfax: no such file');
same(Cli::run($m, ['sendfax'], [], $say), 2, 'sendfax: usage');
/* the page */
$h = dmodem_get($m, ['view' => 'send']);
ok(strpos($h, 'enctype="multipart/form-data"') !== false && strpos($h, 'name="docs[]"') !== false &&
   strpos($h, '5550106') !== false, 'send page: the form and the queue');
ok(strpos(dmodem_get($m, []), 'view=send') !== false, 'the page: a Send fax tab');
dmodem_post($m, ['action' => 'send_fax', 'number' => 'x']);
ok(isset($m->errors['number']) && $m->message[0] === 'danger', 'send_fax: a bad number refused');
/* the dialplan of a sent fax */
$dp = dmodem_dialplan($m);
ok(strpos($dp, "[app-dmodem-faxout]\n") !== false && strpos($dp, "exten => s,1,NoOp(D-Modem fax job \${FILTER(a-z0-9,\${DMODEMFAXJOB})})\n") !== false,
   'the sent fax\'s context');
ok(strpos($dp, "exten => s,n,DModemSendFax($sdir/out-\${FILTER(a-z0-9,\${DMODEMFAXJOB})}.tif)\n") !== false,
   'the sent fax: DModemSendFax of the job\'s file');
ok(strpos($dp, "exten => failed,1,System(/usr/sbin/fwconsole dmodem faxsent '\${FILTER(a-z0-9,\${DMODEMFAXJOB})}'") !== false,
   'the sent fax: the failed extension');
ok(strpos($dp, "[app-dmodem-faxsent]\n") !== false && strpos($dp, "exten => s,1,System(/usr/sbin/fwconsole dmodem faxsent '\${FILTER(a-z0-9,\${ARG1})}'") !== false,
   'the sent fax\'s hangup handler');
$x = Dialplan::faxOut(array_merge($m->getSettings(), ['engine' => 'ari']), '/usr/sbin/fwconsole');
same($x['s'][2], ['Stasis', "dmodem,sendfax,$sdir/out-\${FILTER(a-z0-9,\${DMODEMFAXJOB})}.tif"],
     'the sent fax with dmodem-ari: Stasis(sendfax)');
/* --- UCP: the Fax widget, its rights in User Manager --- */

require_once __DIR__ . '/ucp_env.php';
require_once __DIR__ . '/../dmodem/ucp/Dmodem.class.php';
$ucp = new FakeUcp();
$um = new FakeUserman();
$um->users = [['id' => 7, 'username' => 'carol', 'email' => 'carol@example.com']];
same($m->ucpFaxAccess(7), ['enabled' => false, 'send' => false, 'recipients' => []], 'UCP: no Ucp, no fax');
FreePBX::$modules['Ucp'] = $ucp;
FreePBX::$modules['Userman'] = $um;
$m->saveRecipient(array_merge(Config::defaultRecipient(), ['id' => 'carolfax', 'name' => 'Carol',
	'email' => '', 'user' => 'carol']));
same($m->ucpFaxAccess(7)['enabled'], false, 'UCP: off by default');
$ucp->groupOf[7] = 1;
$_REQUEST['group'] = 1;
$_POST = ['dmodem_fax_enable' => 'yes', 'dmodem_fax_send' => 'no'];
$m->ucpAddGroup(1, 'userman');
same($m->ucpFaxAccess(7), ['enabled' => true, 'send' => false, 'recipients' => ['carolfax']],
     'UCP: on for the group; the recipient of the user\'s User Manager user');
$_POST = ['dmodem_fax_enable' => 'inherit', 'dmodem_fax_send' => 'yes',
	  'dmodem_fax_recipients' => ['sales', 'nope']];
$m->ucpUpdateUser(7, 'userman');
same($ucp->byId[7]['Dmodem'], ['fax_enable' => null, 'fax_send' => true, 'fax_recipients' => ['sales']],
     'UCP: the user\'s settings (inherit, an unknown recipient dropped)');
same($m->ucpFaxAccess(7), ['enabled' => true, 'send' => true, 'recipients' => ['carolfax', 'sales']],
     'UCP: the user\'s own and the group\'s');
$m->ucpUpdateUser(8, 'otherpage');
ok(!isset($ucp->byId[8]), 'UCP: other pages\' saves left alone');
$pg = $m->ucpConfigPage('user', ['id' => 7], 'showuser');
ok($pg[0]['rawname'] === 'dmodem' && strpos($pg[0]['content'], 'value="sales" selected') !== false &&
   strpos($pg[0]['content'], 'id="dmodem_fax_send_yes" value="yes" checked') !== false &&
   strpos($pg[0]['content'], 'id="dmodem_fax_enable_inherit" value="inherit" checked') !== false,
   'UCP: the User Manager page shows the settings');
ok(strpos($m->ucpConfigPage('group', [], 'showgroup')[0]['content'], 'inherit') === false,
   'UCP: no inherit for a group');
/* the faxes that the user sees */
$udir = sys_get_temp_dir() . '/dmodem-ucp-' . getmypid();
mkdir($udir);
$m->saveSettings(array_merge($m->getSettings(), ['fax_dir' => $udir, 'fax_keep_days' => 0]));
file_put_contents("$udir/20261127-1-5550100-1800001000.1.tif", mh_white_tiff(2));
$m->faxReceived('sales', '1800001000.1', ['status' => 'SUCCESS', 'pages' => '1', 'cid' => '5550100',
	'cidname' => 'ACME'], function () { return true; }, 1800001000);
$m->faxReceived('', '1800001000.2', ['status' => 'SUCCESS'], null, 1800001001);
$r = $m->queueFax(['number' => '5550200', 'cover' => 'yes', 'notify' => 'never'], [], 1800001002, $fakeGs, 7);
$m->runFaxQueue(1800001003);
$m->faxSent($r['id'], '1800001004.1', 'SUCCESS', 1, 9600, 'OK', '', null, 1800001004);
$r = $m->queueFax(['number' => '5550201', 'cover' => 'yes', 'notify' => 'never'], [], 1800001005, $fakeGs, 0);
$other = $r['id'];
same(array_keys($m->ucpFaxes(7)), ['1800001004.1', '1800001000.1'],
     'UCP: the user\'s faxes: its recipients\' and its own sent ones');
$m->saveSettings(['fax_block' => '5550999']);
$m->faxReceived('sales', '1800001000.3', ['status' => 'SUCCESS', 'cid' => '5550999'],
	function () { return true; }, 1800001006);
ok(!isset($m->ucpFaxes(7)['1800001000.3']) && $m->getFax('1800001000.3')['blocked'],
   'UCP: no blocked fax');
$m->deleteFax('1800001000.3');
$m->saveSettings(['fax_block' => '']);
same($m->ucpFaxJobs(7), [], 'UCP: no fax of the user waits');
/* the UCP module */
$uu = new \UCP\FakeUser();
$uu->user = ['id' => 7, 'username' => 'carol'];
\UCP\Modules::$ucp = (object)['FreePBX' => $m->FreePBX, 'User' => $uu];
FreePBX::$modules['Dmodem'] = $m;
$w = new \UCP\Modules\Dmodem(null);
/* the stand-in of Ghostscript: the widget runs no real command */
$w->run = $fakeGs;
same($w->getWidgetList()['list']['fax']['display'], 'Fax', 'UCP widget: in the list');
$h = $w->getWidgetDisplay('fax')['html'];
ok(strpos($h, 'data-dmodem-tab="inbox"') !== false && strpos($h, 'data-uniqueid="1800001000.1"') !== false &&
   strpos($h, '5550200') !== false && strpos($h, 'data-dmodem-pane="send"') !== false &&
   strpos($h, '1800001000.2') === false, 'UCP widget: the inbox, the sent fax, the send form; not others\' faxes');
same($w->poll()['latest']['uniqueid'], '1800001000.1', 'UCP widget: the poll gives the newest received fax');
ok($w->ajaxRequest('faxsend', []) && $w->ajaxRequest('faxpdf', []) && !$w->ajaxRequest('other', []),
   'UCP widget: the commands');
$_REQUEST = ['command' => 'faxtiff', 'uniqueid' => '1800001000.1'];
ob_start();
$w->ajaxCustomHandler();
same(ob_get_clean(), mh_white_tiff(2), 'UCP widget: the TIFF of a fax of the user');
$_REQUEST = ['command' => 'faxtiff', 'uniqueid' => '1800001000.2'];
ob_start();
$w->ajaxCustomHandler();
ok(strpos(ob_get_clean(), 'No such fax') !== false, 'UCP widget: not the TIFF of another fax');
$_REQUEST = ['command' => 'faxdelete'];
$_POST = ['uniqueid' => '1800001000.2'];
ok(!$w->ajaxHandler()['status'] && $m->getFax('1800001000.2') !== null, 'UCP widget: not another\'s fax deleted');
$_REQUEST = ['command' => 'faxcancel'];
$_POST = ['job' => $other];
ok(!$w->ajaxHandler()['status'] && $m->getFaxJob($other) !== null, 'UCP widget: not another\'s fax cancelled');
$_REQUEST = ['command' => 'faxforward'];
$_POST = ['uniqueid' => '1800001000.1', 'number' => '5550300'];
$fw = $w->ajaxHandler();
$mine = $m->ucpFaxJobs(7);
ok(count($mine) === 1 && reset($mine)['number'] === '5550300' && strpos($fw['html'], '5550300') !== false,
   'UCP widget: a fax forwarded, in the user\'s outgoing faxes');
$_REQUEST = ['command' => 'faxcancel'];
$_POST = ['job' => key($mine)];
ok(reset($mine)['state'] !== 'sending' || !$w->ajaxHandler()['status'],
   'UCP widget: not a fax cancelled while it is on a call');
/* the try fails and the job waits for its next try: now it cancels */
$m->faxSent(key($mine), '1800002000.1', 'FAILED', 0, 0, 'T30_ERR', '', null, 1800002000);
ok($w->ajaxHandler()['status'] && $m->ucpFaxJobs(7) === [], 'UCP widget: the user\'s fax cancelled');
$_REQUEST = ['command' => 'faxdelete'];
$_POST = ['uniqueid' => '1800001000.1'];
ok($w->ajaxHandler()['status'] && $m->getFax('1800001000.1') === null, 'UCP widget: the user\'s fax deleted');
$ucp->byId[7]['Dmodem']['fax_send'] = false;
ok(!$w->ajaxRequest('faxsend', []) && !$w->ajaxRequest('faxforward', []) && $w->ajaxRequest('faxlist', []),
   'UCP widget: no send right, no send');
ok(strpos($w->widgetHtml(), 'data-dmodem-pane="send"') === false, 'UCP widget: no send form without the right');
$ucp->byGid[1]['Dmodem']['fax_enable'] = false;
ok($w->getWidgetList() === [] && !$w->ajaxRequest('faxlist', []), 'UCP widget: off, nothing');
$m->cancelFax($other);
unset(FreePBX::$modules['Ucp'], FreePBX::$modules['Userman']);
$m->deleteRecipient('carolfax');
array_map('unlink', glob("$udir/*"));
rmdir($udir);
$_REQUEST = $_POST = [];

/* the job each minute */
$fj = new FakeJob();
FreePBX::$modules['Job'] = $fj;
$m->install();
same($fj->jobs['dmodem/faxqueue'][0], '/usr/sbin/fwconsole dmodem faxqueue', 'install: the fax queue job');
same($fj->jobs['dmodem/faxqueue'][1], '* * * * *', 'install: each minute');
$m->uninstall();
same($fj->jobs, [], 'uninstall: the job gone');
unset(FreePBX::$modules['Job']);
$m->install();
array_map('unlink', glob("$sdir/*"));
rmdir($sdir);

/* --- uninstall --- */

$m->uninstall();
same($m->getProfiles(), [], 'uninstall: the profiles gone');

echo "freepbx unit: $passed passed, $failed failed\n";
exit($failed ? 1 : 0);
