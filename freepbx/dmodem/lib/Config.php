<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Neo-Desktop
//
// Config.php - the module's settings and profiles: their defaults, their
// checks, and dmodem.conf made from them (as app_dmodem and dmodem-ari
// read it: asterisk/dmodem.conf.sample).

namespace FreePBX\modules\Dmodem;

class Config
{
	/* where a profile's data goes (dmodem.conf mode) */
	const MODES = ['tcp', 'unix', 'exec'];
	/* the network's G.711 law (auto: the channel's) */
	const LAWS = ['auto', 'ulaw', 'alaw'];
	/* what answers the calls: app_dmodem's DModem() in Asterisk, or
	   dmodem-ari beside it through Stasis() */
	const ENGINES = ['app', 'ari'];
	/* the fax rates T.30 can be held to */
	const FAX_RATES = [14400, 12000, 9600, 7200, 4800, 2400];
	/* names a profile can't have: dmodem.conf's [general], and the
	   commands of dmodem-ari's Stasis() and the fax destination */
	const RESERVED = ['general', 'receivefax', 'sendfax'];

	/* the settings of a new install */
	public static function defaultSettings()
	{
		return [
			'engine' => 'app',
			'debug' => 0,
			'fax_dir' => '/var/spool/asterisk/fax',
			'fax_ident' => '',
			'fax_max_rate' => 14400,
			'fax_ecm' => true,
			'fax_t38' => true,
			'fax_destination' => true,
			'ari_url' => 'http://127.0.0.1:8088',
			'ari_user' => 'dmodem',
			'ari_password' => '',
			'ari_app' => 'dmodem',
			'ari_t38' => false,
			'sip_endpoint' => 'dmodem',
			'sip_bind' => '127.0.0.1:5090',
		];
	}

	/* a new profile's fields */
	public static function defaultProfile()
	{
		return [
			'name' => '',
			'description' => '',
			'mode' => 'tcp',
			'host' => '127.0.0.1',
			'port' => 2323,
			'path' => '',
			'command' => '',
			'header' => false,
			'law' => 'auto',
			'init' => '',
			'answer' => '',
			'timeout' => 60,
			'fax' => true,
		];
	}

	/* a checkbox or yes/no value as a bool */
	private static function flag($v)
	{
		if (is_bool($v))
			return $v;
		return in_array(strtolower(trim((string)$v)),
				['1', 'yes', 'on', 'true'], true);
	}

	/* a form's (or the store's) values over the defaults, as the store
	   keeps them: strings trimmed, numbers as ints, flags as bools.
	   Fields the defaults don't have are dropped */
	private static function normalize(array $in, array $defaults)
	{
		$out = [];
		foreach ($defaults as $k => $def) {
			if (!array_key_exists($k, $in)) {
				/* an unchecked checkbox is not posted at all */
				$out[$k] = is_bool($def) ? false : $def;
				continue;
			}
			$v = $in[$k];
			if (is_bool($def))
				$out[$k] = self::flag($v);
			elseif (is_int($def))
				$out[$k] = is_numeric($v) ? (int)$v : $v;
			else
				$out[$k] = trim((string)$v);
		}
		return $out;
	}

	/* a profile from the form: see normalize() */
	public static function normalizeProfile(array $in)
	{
		$p = self::normalize($in, self::defaultProfile());
		$p['name'] = strtolower($p['name']);
		return $p;
	}

	/* the settings from the form: see normalize() */
	public static function normalizeSettings(array $in)
	{
		return self::normalize($in, self::defaultSettings());
	}

	/* true if s holds a character that would end or break a dmodem.conf
	   line (a newline, a NUL) */
	private static function breaksLine($s)
	{
		return preg_match('/[\x00\r\n]/', (string)$s) === 1;
	}

	/* an int in [lo, hi] */
	private static function intIn($v, $lo, $hi)
	{
		return is_int($v) && $v >= $lo && $v <= $hi;
	}

	/* what is wrong with profile p, field => message (empty: nothing);
	   taken: the names of the other profiles */
	public static function validateProfile(array $p, array $taken = [])
	{
		$e = [];
		if (!preg_match('/^[a-z0-9][a-z0-9_-]{0,39}$/', $p['name']))
			$e['name'] = _('A name of 1 to 40 letters, digits, - and _, starting with a letter or digit');
		elseif (in_array($p['name'], self::RESERVED, true))
			$e['name'] = sprintf(_('"%s" is reserved'), $p['name']);
		elseif (in_array($p['name'], $taken, true))
			$e['name'] = _('There is a profile of that name already');
		if (self::breaksLine($p['description']) ||
		    strlen($p['description']) > 100)
			$e['description'] = _('One line of 100 characters at most');
		if (!in_array($p['mode'], self::MODES, true))
			$e['mode'] = _('tcp, unix or exec');
		if ($p['mode'] === 'tcp') {
			if (!preg_match('/^[A-Za-z0-9._:-]{1,253}$/', $p['host']))
				$e['host'] = _('A host name or an IP address');
			if (!self::intIn($p['port'], 1, 65535))
				$e['port'] = _('A port from 1 to 65535');
		} elseif ($p['mode'] === 'unix') {
			if (!preg_match('#^/[^\x00\r\n;]{1,200}$#', $p['path']))
				$e['path'] = _('An absolute path to the socket');
		} elseif ($p['mode'] === 'exec') {
			if ($p['command'] === '' || self::breaksLine($p['command']))
				$e['command'] = _('The program to run, on one line');
		}
		if (!in_array($p['law'], self::LAWS, true))
			$e['law'] = _('auto, ulaw or alaw');
		foreach (['init', 'answer'] as $k)
			if (self::breaksLine($p[$k]) || strlen($p[$k]) > 200)
				$e[$k] = _('One line of AT commands, 200 characters at most');
			elseif ($p[$k] !== '' && stripos($p[$k], 'AT') !== 0)
				$e[$k] = _('AT commands, starting with AT');
		if (!self::intIn($p['timeout'], 5, 600))
			$e['timeout'] = _('From 5 to 600 seconds');
		return $e;
	}

	/* what is wrong with settings s, field => message */
	public static function validateSettings(array $s)
	{
		$e = [];
		if (!in_array($s['engine'], self::ENGINES, true))
			$e['engine'] = _('app or ari');
		if (!self::intIn($s['debug'], 0, 3))
			$e['debug'] = _('0 to 3');
		if (!preg_match('#^/[^\x00\r\n;]{0,200}$#', $s['fax_dir']))
			$e['fax_dir'] = _('An absolute path');
		if (!preg_match('/^[0-9+ ]{0,20}$/', $s['fax_ident']))
			$e['fax_ident'] = _('Up to 20 digits, + and spaces');
		if (!in_array($s['fax_max_rate'], self::FAX_RATES, true))
			$e['fax_max_rate'] = _('One of the fax rates');
		if ($s['engine'] === 'ari') {
			if (!preg_match('#^https?://[^\s;]{1,200}$#', $s['ari_url']))
				$e['ari_url'] = _('http://host:port of Asterisk\'s HTTP server');
			if (!preg_match('/^[A-Za-z0-9._-]{1,64}$/', $s['ari_user']))
				$e['ari_user'] = _('The ARI user of ari.conf');
			if (self::breaksLine($s['ari_password']) ||
			    strlen($s['ari_password']) > 128)
				$e['ari_password'] = _('One line, 128 characters at most');
			if (!preg_match('/^[A-Za-z0-9._-]{1,64}$/', $s['ari_app']))
				$e['ari_app'] = _('Letters, digits, ., - and _');
			if ($s['ari_t38']) {
				if (!preg_match('/^[A-Za-z0-9._-]{1,64}$/',
						$s['sip_endpoint']))
					$e['sip_endpoint'] = _('A PJSIP endpoint name');
				if (!preg_match('/^[A-Za-z0-9.:\[\]-]{1,64}:[0-9]{1,5}$/',
						$s['sip_bind']))
					$e['sip_bind'] = _('address:port');
			}
		}
		return $e;
	}

	/* v as a dmodem.conf value: its ';' written '\;' (else the rest is
	   a comment), the line breaks gone */
	public static function escape($v)
	{
		if (is_bool($v))
			return $v ? 'yes' : 'no';
		$v = preg_replace('/[\x00\r\n]+/', ' ', (string)$v);
		return str_replace(';', '\\;', $v);
	}

	/* "name = value" */
	private static function line($name, $value)
	{
		return $name . ' = ' . self::escape($value);
	}

	/* dmodem.conf of the settings and the profiles (name => profile),
	   the profiles in name order */
	public static function render(array $settings, array $profiles)
	{
		$s = array_merge(self::defaultSettings(), $settings);
		$o = [];
		$o[] = '; dmodem.conf: the profiles of DModem() (app_dmodem) and dmodem-ari';
		$o[] = '; Made by the FreePBX module D-Modem (Applications > D-Modem):';
		$o[] = '; edit the profiles there, not here.';
		$o[] = '';
		$o[] = '[general]';
		$o[] = self::line('debug', (int)$s['debug']);
		$o[] = self::line('fax_dir', $s['fax_dir']);
		if ($s['fax_ident'] !== '')
			$o[] = self::line('fax_ident', $s['fax_ident']);
		$o[] = self::line('fax_max_rate', (int)$s['fax_max_rate']);
		$o[] = self::line('fax_ecm', (bool)$s['fax_ecm']);
		$o[] = self::line('fax_t38', (bool)$s['fax_t38']);
		if ($s['engine'] === 'ari') {
			$o[] = self::line('ari_url', $s['ari_url']);
			$o[] = self::line('ari_user', $s['ari_user']);
			$o[] = self::line('ari_password', $s['ari_password']);
			$o[] = self::line('ari_app', $s['ari_app']);
			if ($s['ari_t38']) {
				$o[] = self::line('t38', true);
				$o[] = self::line('sip_endpoint', $s['sip_endpoint']);
				$o[] = self::line('sip_bind', $s['sip_bind']);
			}
		}
		ksort($profiles, SORT_STRING);
		foreach ($profiles as $name => $p) {
			$p = array_merge(self::defaultProfile(), $p);
			$o[] = '';
			if ($p['description'] !== '')
				$o[] = '; ' . preg_replace('/[\x00\r\n]+/', ' ',
							   $p['description']);
			$o[] = '[' . $name . ']';
			$o[] = self::line('mode', $p['mode']);
			if ($p['mode'] === 'tcp') {
				$o[] = self::line('host', $p['host']);
				$o[] = self::line('port', (int)$p['port']);
			} elseif ($p['mode'] === 'unix') {
				$o[] = self::line('path', $p['path']);
			} else {
				$o[] = self::line('command', $p['command']);
			}
			if ($p['mode'] !== 'exec')
				$o[] = self::line('header', (bool)$p['header']);
			$o[] = self::line('law', $p['law']);
			if ($p['init'] !== '')
				$o[] = self::line('init', $p['init']);
			if ($p['answer'] !== '')
				$o[] = self::line('answer', $p['answer']);
			$o[] = self::line('timeout', (int)$p['timeout']);
			$o[] = self::line('fax', (bool)$p['fax']);
		}
		return implode("\n", $o) . "\n";
	}
}
