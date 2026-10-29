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
		 'fax_max_rate' => '9600', 'fax_destination' => 'yes',
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
		 'fax_dir' => "relative\npath", 'fax_ident' => 'abc', 'fax_max_rate' => '1']);
foreach (['engine', 'debug', 'fax_dir', 'fax_ident', 'fax_max_rate'] as $k)
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
		 'law' => 'auto', 'timeout' => '45']);
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
	'description' => ['description' => "two\nlines"],
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
same(array_keys($c), ['general', 'bbs', 'ppp'], 'dmodem.conf: [general] and the profiles in order');
same($c['bbs']['answer'], 'AT+MS=132;A', 'a ; in a value survives (written \\;)');
same($c['ppp']['command'], 'exec /usr/sbin/pppd nodetach noauth local; echo x', 'the command with its ;');
same($c['bbs']['host'] . ':' . $c['bbs']['port'], '127.0.0.1:2323', 'tcp host and port');
same($c['bbs']['law'], 'ulaw', 'law');
same($c['bbs']['fax'], 'yes', 'fax yes');
same($c['general']['fax_ecm'], 'no', 'fax_ecm no');
same($c['general']['fax_ident'], '+1 555 0100', 'fax_ident');
same($c['general']['fax_max_rate'], '9600', 'fax_max_rate');
ok(!isset($c['ppp']['header']), 'exec has no header');
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
ok(strpos($dp, "exten => receivefax,n,DModemReceiveFax(/var/spool/asterisk/fax/\${STRFTIME(\${EPOCH},,%Y%m%d-%H%M%S)}-\${FILTER(0-9A-Za-z+,\${CALLERID(num)})}-\${UNIQUEID}.tif)\n") !== false,
   'receivefax runs DModemReceiveFax into fax_dir');
same(substr_count($dp, ',n,Hangup()'), 3, 'each extension hangs up');
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

/* --- export, import, backup and restore --- */

$exp = $m->exportData();
same(array_keys($exp), ['format', 'settings', 'profiles'], 'export: its parts');
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
ok(strpos(json_encode($rs->logged), '1 profiles restored') !== false, 'restore: logged');
FreePBX_Helpers::$kv = $saved;
FreePBX::$modules['Dmodem'] = $m;

/* --- uninstall --- */

$m->uninstall();
same($m->getProfiles(), [], 'uninstall: the profiles gone');

echo "freepbx unit: $passed passed, $failed failed\n";
exit($failed ? 1 : 0);
