<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Neo-Desktop
//
// Dmodem.class.php - the FreePBX module (BMO) of D-Modem: profiles of
// DModem() (app_dmodem) or dmodem-ari kept in the module's store,
// dmodem.conf written from them on Apply Config, the app-dmodem context
// with a destination for each, and a check of the PJSIP endpoints for
// modem calls.

namespace FreePBX\modules;

require_once __DIR__ . '/lib/Config.php';
require_once __DIR__ . '/lib/Dialplan.php';
require_once __DIR__ . '/lib/LineCheck.php';

use FreePBX\modules\Dmodem\Config;
use FreePBX\modules\Dmodem\Dialplan;
use FreePBX\modules\Dmodem\LineCheck;

class Dmodem extends \FreePBX_Helpers implements \BMO
{
	/* the store's groups (kvstore ids) */
	const SETTINGS = 'settings';
	const PROFILES = 'profiles';
	/* the endpoints the check reads at most */
	const MAX_ENDPOINTS = 200;

	public $FreePBX;
	/* the last form's errors, field => message, and its values */
	public $errors = [];
	public $posted = null;
	/* the last action's message for the page: [level, text] */
	public $message = null;

	/* freepbx: FreePBX's object */
	public function __construct($freepbx = null)
	{
		if ($freepbx === null)
			throw new \Exception('Not given a FreePBX object');
		$this->FreePBX = $freepbx;
	}

	/* s for HTML */
	public static function h($s)
	{
		return htmlspecialchars((string)$s, ENT_QUOTES, 'UTF-8');
	}

	/* the default settings, kept ones left alone */
	public function install()
	{
		$have = $this->getAll(self::SETTINGS);
		foreach (Config::defaultSettings() as $k => $v)
			if (!is_array($have) || !array_key_exists($k, $have))
				$this->setConfig($k, self::stored($v), self::SETTINGS);
	}

	/* v as the store keeps it: a bool as yes or no, since the store
	   takes false as "delete" (and the default would come back) */
	private static function stored($v)
	{
		return is_bool($v) ? ($v ? 'yes' : 'no') : $v;
	}

	/* the store emptied (dmodem.conf is left for the next reload to
	   drop: FreePBX no longer writes it) */
	public function uninstall()
	{
		$this->delById(self::SETTINGS);
		$this->delById(self::PROFILES);
	}

	/* settings over the defaults */
	public function getSettings()
	{
		$have = $this->getAll(self::SETTINGS);
		return Config::normalizeSettings(array_merge(
			Config::defaultSettings(), is_array($have) ? $have : []));
	}

	/* the settings s (checked) kept */
	public function saveSettings(array $s)
	{
		foreach ($s as $k => $v)
			$this->setConfig($k, self::stored($v), self::SETTINGS);
	}

	/* name => profile, in name order */
	public function getProfiles()
	{
		$all = $this->getAll(self::PROFILES);
		$out = [];
		if (is_array($all))
			foreach ($all as $name => $p)
				if (is_array($p))
					$out[$name] = Config::normalizeProfile(
						array_merge(Config::defaultProfile(), $p,
							    ['name' => $name]));
		ksort($out, SORT_STRING);
		return $out;
	}

	/* profile name, null if there is none */
	public function getProfile($name)
	{
		$all = $this->getProfiles();
		return isset($all[$name]) ? $all[$name] : null;
	}

	/* profile p (checked) kept under its name */
	public function saveProfile(array $p)
	{
		$this->setConfig($p['name'], $p, self::PROFILES);
	}

	/* profile name dropped */
	public function deleteProfile($name)
	{
		$this->setConfig($name, false, self::PROFILES);
	}

	/* the destination of profile (or extension) name */
	public static function destination($name)
	{
		return Dialplan::CONTEXT . ',' . $name . ',1';
	}

	/* what uses destination dest (FreePBX's other modules), empty if
	   nothing or unknown */
	private function destinationUsers($dest)
	{
		if (!function_exists('framework_check_destination_usage'))
			return [];
		$u = framework_check_destination_usage($dest);
		return is_array($u) ? $u : [];
	}

	/* the form posted for page display=dmodem: a profile saved or
	   deleted, the settings saved */
	public function doConfigPageInit($page)
	{
		$req = $_REQUEST;
		$action = isset($req['action']) ? $req['action'] : '';
		if ($action === 'delete' && isset($req['profile'])) {
			$name = (string)$req['profile'];
			if ($this->getProfile($name) === null)
				return;
			if ($this->destinationUsers(self::destination($name))) {
				$this->message = ['danger', sprintf(_('Profile %s is the destination of a route or another object: point those elsewhere first'), $name)];
				$_REQUEST['view'] = 'form';
				return;
			}
			$this->deleteProfile($name);
			$this->reload();
			$this->message = ['success', sprintf(_('Profile %s deleted'), $name)];
			$_REQUEST['view'] = '';
			return;
		}
		if ($action === 'save_profile') {
			$p = Config::normalizeProfile($req);
			$old = isset($req['original']) ? (string)$req['original'] : '';
			if ($old !== '') {
				/* a profile keeps its name: it is its destination */
				$p['name'] = $old;
				$taken = [];
			} else {
				$taken = array_keys($this->getProfiles());
			}
			$this->errors = Config::validateProfile($p, $taken);
			if ($this->errors) {
				$this->posted = $p;
				$_REQUEST['view'] = 'form';
				return;
			}
			$this->saveProfile($p);
			$this->reload();
			$this->message = ['success', sprintf(_('Profile %s saved'), $p['name'])];
			$_REQUEST['view'] = '';
			return;
		}
		if ($action === 'save_settings') {
			$s = Config::normalizeSettings($req);
			/* a password left empty keeps the one there is */
			if ($s['ari_password'] === '')
				$s['ari_password'] = $this->getSettings()['ari_password'];
			$this->errors = Config::validateSettings($s);
			if ($this->errors) {
				$this->posted = $s;
				$_REQUEST['view'] = 'settings';
				return;
			}
			$this->saveSettings($s);
			$this->reload();
			$this->message = ['success', _('Settings saved')];
			return;
		}
	}

	/* the Apply Config button shown */
	private function reload()
	{
		if (function_exists('needreload'))
			needreload();
	}

	/* the action bar's buttons for the page in request */
	public function getActionBar($request)
	{
		if (!isset($request['display']) || $request['display'] !== 'dmodem')
			return [];
		$view = isset($request['view']) ? $request['view'] : '';
		if ($view !== 'form' && $view !== 'settings')
			return [];
		$b = [
			'reset' => ['name' => 'reset', 'id' => 'reset',
				    'value' => _('Reset')],
			'submit' => ['name' => 'submit', 'id' => 'submit',
				     'value' => _('Submit')],
		];
		if ($view === 'form' && !empty($request['profile']))
			$b['delete'] = ['name' => 'delete', 'id' => 'delete',
					'value' => _('Delete')];
		return $b;
	}

	/* view (views/<view>.php) with vars, as HTML */
	public function view($view, array $vars = [])
	{
		if (!preg_match('/^[a-z_]+$/', $view))
			return '';
		$vars['dmodem'] = $this;
		extract($vars, EXTR_SKIP);
		ob_start();
		include __DIR__ . '/views/' . $view . '.php';
		return ob_get_clean();
	}

	/* the page for the request */
	public function showPage()
	{
		$view = isset($_REQUEST['view']) ? $_REQUEST['view'] : '';
		$settings = $this->getSettings();
		$common = ['view' => $view, 'message' => $this->message,
			   'settings' => $settings];
		switch ($view) {
		case 'form':
			$name = isset($_REQUEST['profile']) ? (string)$_REQUEST['profile'] : '';
			$p = $this->posted !== null ? $this->posted :
				($name !== '' ? $this->getProfile($name) : null);
			$editing = $name !== '' && $this->getProfile($name) !== null;
			if ($p === null)
				$p = Config::defaultProfile();
			$body = $this->view('profile', ['profile' => $p,
				'editing' => $editing, 'errors' => $this->errors]);
			break;
		case 'settings':
			$s = $this->posted !== null ? $this->posted : $settings;
			$body = $this->view('settings', ['s' => $s,
				'errors' => $this->errors]);
			break;
		case 'check':
			$body = $this->view('check', ['check' => $this->check($settings)]);
			break;
		case 'dialplan':
			$body = $this->view('dialplan', [
				'conf' => Config::render($settings, $this->getProfiles()),
				'dialplan' => Dialplan::render(Dialplan::build(
					$settings, $this->getProfiles()))]);
			break;
		default:
			$common['view'] = '';
			$body = $this->view('profiles', ['profiles' => $this->getProfiles()]);
		}
		return $this->view('main', $common + ['body' => $body]);
	}

	/* dmodem.conf for FreePBX to write on Apply Config */
	public function genConfig()
	{
		return ['dmodem.conf' => Config::render($this->getSettings(),
							$this->getProfiles())];
	}

	/* the files of genConfig() written into Asterisk's etc */
	public function writeConfig($conf)
	{
		$this->FreePBX->WriteConfig($conf);
	}

	/* this module adds to the dialplan */
	public static function myDialplanHooks()
	{
		return 500;
	}

	/* app-dmodem into FreePBX's dialplan ext */
	public function doDialplanHook(&$ext, $engine, $priority)
	{
		require_once __DIR__ . '/lib/Apps.php';
		$context = Dialplan::build($this->getSettings(), $this->getProfiles());
		foreach ($context as $exten => $steps)
			foreach ($steps as $st)
				$ext->add(Dialplan::CONTEXT, $exten, '',
					  new \ext_dmodem_app($st[0], $st[1]));
	}

	/* the destinations for routes, IVRs and the like */
	public function destinations()
	{
		$d = [];
		foreach ($this->getProfiles() as $name => $p)
			$d[] = [
				'destination' => self::destination($name),
				'description' => $p['description'] !== '' ?
					$name . ' (' . $p['description'] . ')' : $name,
				'category' => 'D-Modem',
			];
		if ($this->getSettings()['fax_destination'])
			$d[] = [
				'destination' => self::destination(Dialplan::FAX_EXTEN),
				'description' => _('Receive a fax'),
				'category' => 'D-Modem',
			];
		return $d;
	}

	/* what destination dest is, for the pages that show it; null if it
	   is none of ours */
	public function destinationInfo($dest)
	{
		$pre = Dialplan::CONTEXT . ',';
		if (strpos($dest, $pre) !== 0)
			return null;
		$parts = explode(',', $dest);
		$name = isset($parts[1]) ? $parts[1] : '';
		if ($name === Dialplan::FAX_EXTEN)
			return ['description' => _('D-Modem: receive a fax'),
				'edit_url' => 'config.php?display=dmodem&view=settings'];
		if ($this->getProfile($name) === null)
			return [];
		return ['description' => sprintf(_('D-Modem profile %s'), $name),
			'edit_url' => 'config.php?display=dmodem&view=form&profile=' .
				urlencode($name)];
	}

	/* the output of CLI command cmd through the manager, null if there
	   is no manager */
	private function cli($cmd)
	{
		$am = isset($this->FreePBX->astman) ? $this->FreePBX->astman : null;
		if (!$am || (method_exists($am, 'connected') && !$am->connected()))
			return null;
		$r = method_exists($am, 'Command') ? $am->Command($cmd) :
			$am->send_request('Command', ['Command' => $cmd]);
		if (!is_array($r))
			return '';
		foreach (['data', 'Output'] as $k)
			if (isset($r[$k]))
				return is_array($r[$k]) ? implode("\n", $r[$k]) : (string)$r[$k];
		return '';
	}

	/* the check page's findings: the engine's state, and each PJSIP
	   endpoint's settings and what they mean */
	public function check(array $settings)
	{
		$c = ['manager' => true, 'engine' => $settings['engine'],
		      'engine_ok' => null, 'endpoints' => []];
		if ($settings['engine'] === 'ari') {
			$out = $this->cli('ari show apps');
			$c['engine_ok'] = $out === null ? null :
				LineCheck::ariAppRegistered($out, $settings['ari_app']);
		} else {
			$out = $this->cli('module show like app_dmodem');
			$c['engine_ok'] = $out === null ? null :
				LineCheck::moduleRunning($out, 'app_dmodem');
		}
		if ($out === null) {
			$c['manager'] = false;
			return $c;
		}
		$names = LineCheck::endpointNames($this->cli('pjsip show endpoints'));
		foreach (array_slice($names, 0, self::MAX_ENDPOINTS) as $n) {
			$s = LineCheck::endpointSettings(
				$this->cli('pjsip show endpoint ' . $n));
			$r = LineCheck::evaluate($s);
			$c['endpoints'][$n] = ['settings' => $s, 'findings' => $r,
					       'worst' => LineCheck::worst($r)];
		}
		return $c;
	}
}
