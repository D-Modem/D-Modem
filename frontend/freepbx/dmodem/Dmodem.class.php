<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// Dmodem.class.php - the FreePBX module (BMO) of D-Modem: profiles of
// DModem() (app_dmodem) or dmodem-ari kept in the module's store,
// dmodem.conf written from them on Apply Config, the app-dmodem context
// with a destination for each, calls out with a profile, the settings'
// export and import (Backup.php, Restore.php, fwconsole dmodem), the fax
// recipients (fax-to-email: FaxMail), and a check of the PJSIP endpoints
// for modem calls.

namespace FreePBX\modules;

require_once __DIR__ . '/lib/Config.php';
require_once __DIR__ . '/lib/Dialplan.php';
require_once __DIR__ . '/lib/LineCheck.php';
require_once __DIR__ . '/lib/Dialout.php';
require_once __DIR__ . '/lib/Cli.php';
require_once __DIR__ . '/lib/FaxMail.php';
require_once __DIR__ . '/lib/FaxLog.php';
require_once __DIR__ . '/lib/FaxSend.php';
require_once __DIR__ . '/lib/FaxCover.php';
require_once __DIR__ . '/lib/FaxJunk.php';

use FreePBX\modules\Dmodem\Config;
use FreePBX\modules\Dmodem\Dialplan;
use FreePBX\modules\Dmodem\LineCheck;
use FreePBX\modules\Dmodem\Dialout;
use FreePBX\modules\Dmodem\FaxMail;
use FreePBX\modules\Dmodem\FaxLog;
use FreePBX\modules\Dmodem\FaxPdf;
use FreePBX\modules\Dmodem\FaxSend;
use FreePBX\modules\Dmodem\FaxJunk;

class Dmodem extends \FreePBX_Helpers implements \BMO
{
	/* the store's groups (kvstore ids) */
	const SETTINGS = 'settings';
	const PROFILES = 'profiles';
	const RECIPIENTS = 'recipients';
	const DIDFAX = 'didfax';
	const FAXLOG = 'faxlog';
	const FAXJOBS = 'faxjobs';
	/* the endpoints the check reads at most */
	const MAX_ENDPOINTS = 200;
	/* exportData()'s format */
	const EXPORT_FORMAT = 1;

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
		/* the fax queue each minute (FreePBX's jobs: fwconsole job) */
		$job = $this->jobs();
		if ($job !== null)
			$job->addCommand('dmodem', 'faxqueue',
					 $this->fwconsole() . ' dmodem faxqueue', '* * * * *', 600);
	}

	/* FreePBX's jobs (fwconsole job), null if this FreePBX has none */
	private function jobs()
	{
		try {
			$j = $this->FreePBX->Job;
		} catch (\Throwable $e) {
			return null;
		}
		return is_object($j) && method_exists($j, 'addCommand') ? $j : null;
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
		$this->delById(self::RECIPIENTS);
		$this->delById(self::DIDFAX);
		$this->delById(self::FAXLOG);
		$this->delById(self::FAXJOBS);
		$job = $this->jobs();
		if ($job !== null)
			$job->remove('dmodem', 'faxqueue');
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

	/* id => fax recipient, in id order */
	public function getRecipients()
	{
		$all = $this->getAll(self::RECIPIENTS);
		$out = [];
		if (is_array($all))
			foreach ($all as $id => $r)
				if (is_array($r))
					$out[$id] = Config::normalizeRecipient(
						array_merge(Config::defaultRecipient(), $r,
							    ['id' => $id]));
		ksort($out, SORT_STRING);
		return $out;
	}

	/* recipient id, null if there is none */
	public function getRecipient($id)
	{
		$all = $this->getRecipients();
		return isset($all[$id]) ? $all[$id] : null;
	}

	/* recipient r (checked) kept under its id */
	public function saveRecipient(array $r)
	{
		$this->setConfig($r['id'], $r, self::RECIPIENTS);
	}

	/* recipient id dropped */
	public function deleteRecipient($id)
	{
		$this->setConfig($id, false, self::RECIPIENTS);
	}

	/* the fax detection of the inbound routes: <DID>/<caller ID number>
	   => its settings (Config::defaultDidFax()), in key order */
	public function getDidFaxes()
	{
		$all = $this->getAll(self::DIDFAX);
		$out = [];
		if (is_array($all))
			foreach ($all as $k => $f)
				if (is_array($f))
					$out[(string)$k] = Config::normalizeDidFax(
						array_merge(Config::defaultDidFax(), $f));
		ksort($out, SORT_STRING);
		return $out;
	}

	/* the fax detection of the inbound route of key (Config::didKey()):
	   its settings, the defaults if it has none */
	public function getDidFax($key)
	{
		$all = $this->getDidFaxes();
		return isset($all[$key]) ? $all[$key] : Config::defaultDidFax();
	}

	/* the fax detection f of the inbound route of key kept; off: none
	   kept */
	public function saveDidFax($key, array $f)
	{
		$this->setConfig($key, $f['detect'] ? $f : false, self::DIDFAX);
	}

	/* the pages of other modules that this one adds to: the inbound
	   routes (their fax detection) */
	public static function myConfigPageInits()
	{
		return ['did'];
	}

	/* the inbound route form posted (Core has saved the route): its fax
	   detection kept under the route's DID and caller ID number, moved
	   with the route, or dropped with it */
	private function didPageInit(array $req)
	{
		$action = isset($req['action']) ? $req['action'] : '';
		if ($action === 'delIncoming') {
			$parts = explode('/', isset($req['extdisplay']) ? (string)$req['extdisplay'] : '', 2);
			$key = Config::didKey($parts[0], isset($parts[1]) ? $parts[1] : '');
			if ($key !== null) {
				$this->saveDidFax($key, Config::defaultDidFax());
				$this->reload();
			}
			return;
		}
		if (($action !== 'addIncoming' && $action !== 'edtIncoming') ||
		    empty($req['dmodem_fax_form']))
			return;
		$key = Config::didKey(isset($req['extension']) ? $req['extension'] : '',
				      isset($req['cidnum']) ? $req['cidnum'] : '');
		if ($key === null)
			return;
		if ($action === 'edtIncoming' && isset($req['old_extension'])) {
			$old = Config::didKey($req['old_extension'],
					      isset($req['old_cidnum']) ? $req['old_cidnum'] : '');
			if ($old !== null && $old !== $key)
				$this->saveDidFax($old, Config::defaultDidFax());
		}
		$dest = '';
		if (!empty($req['gotodmodemfax'])) {
			$k = $req['gotodmodemfax'] . 'dmodemfax';
			$dest = isset($req[$k]) ? (string)$req[$k] : '';
		} elseif (isset($req['dmodem_fax_dest'])) {
			$dest = (string)$req['dmodem_fax_dest'];
		}
		$f = Config::normalizeDidFax([
			'detect' => isset($req['dmodem_fax_detect']) ? $req['dmodem_fax_detect'] : '',
			'listen' => isset($req['dmodem_fax_listen']) ? $req['dmodem_fax_listen'] : 'both',
			'wait' => isset($req['dmodem_fax_wait']) ? $req['dmodem_fax_wait'] : 4,
			'ring' => isset($req['dmodem_fax_ring']) ? $req['dmodem_fax_ring'] : '',
			'dest' => $dest,
		]);
		/* (a bad value: the route's fax detection stays as it was; the
		   form's own checks stop most of them) */
		if (Config::validateDidFax($f))
			return;
		$this->saveDidFax($key, $f);
		$this->reload();
	}

	/* the D-Modem fax tab of the inbound route page (Core's hookTabs):
	   page the page's display */
	public function didFaxTab($page)
	{
		if ($page !== 'did')
			return [];
		$ext = isset($_REQUEST['extdisplay']) ? (string)$_REQUEST['extdisplay'] : '';
		$parts = explode('/', $ext, 2);
		$key = $ext === '' ? null : Config::didKey($parts[0], isset($parts[1]) ? $parts[1] : '');
		$f = $key === null ? Config::defaultDidFax() : $this->getDidFax($key);
		return [[
			'title' => _('D-Modem fax'),
			'rawname' => 'dmodemfax',
			'content' => $this->view('didfax', ['f' => $f,
				'settings' => $this->getSettings()]),
		]];
	}

	/* the inbound routes whose faxes go to a destination of dest (a
	   list, or one; all if dest is true), as FreePBX's
	   check_destinations has them */
	public function didFaxUsage($dest = true)
	{
		$out = [];
		foreach ($this->getDidFaxes() as $key => $f) {
			if ($dest !== true && !in_array($f['dest'], (array)$dest, true))
				continue;
			$parts = explode('/', $key, 2);
			$out[] = [
				'dest' => $f['dest'],
				'description' => sprintf(_('Inbound route %s: D-Modem fax'), $key),
				'edit_url' => 'config.php?display=did&view=form&extdisplay=' .
					urlencode($key),
			];
		}
		return $out;
	}

	/* User Manager's users with an email address, username => email;
	   null if User Manager is not installed */
	public function usermanUsers()
	{
		$um = null;
		try {
			if (isset($this->FreePBX->Userman))
				$um = $this->FreePBX->Userman;
		} catch (\Throwable $e) {
			$um = null;
		}
		if (!$um || !method_exists($um, 'getAllUsers'))
			return null;
		$out = [];
		foreach ((array)$um->getAllUsers() as $u) {
			$u = (array)$u;
			if (!empty($u['username']) && !empty($u['email']))
				$out[(string)$u['username']] = (string)$u['email'];
		}
		ksort($out, SORT_STRING);
		return $out;
	}

	/* recipient r with its email address: its own, else its User
	   Manager user's */
	public function recipientAddress(array $r)
	{
		if ($r['email'] !== '' || $r['user'] === '')
			return $r;
		$u = $this->usermanUsers();
		$r['email'] = $u !== null && isset($u[$r['user']]) ? $u[$r['user']] : '';
		return $r;
	}

	/* fwconsole dmodem fax2mail: the fax of call uniqueid mailed to
	   recipient id (the call's results in vars: status, pages, cid,
	   remote, cidname); send as FaxMail::send() has it.  [ok, message] */
	public function faxToMail($id, $uniqueid, array $vars, ?callable $send = null)
	{
		$r = $this->getRecipient((string)$id);
		if ($r === null)
			return [false, sprintf(_('No fax recipient %s'), $id)];
		return FaxMail::send($this->getSettings(), $this->recipientAddress($r),
				     $uniqueid, $vars, $send);
	}

	/* fwconsole dmodem fax2mail: the fax of call uniqueid into the
	   fax log, then mailed to recipient id ('' for none: the log only);
	   the call's results in vars (FaxLog::VARS); send as
	   FaxMail::send() has it; now the time (seconds, null: time()).
	   The old entries go (fax_keep_days).  [ok, message] */
	public function faxReceived($id, $uniqueid, array $vars,
				    ?callable $send = null, $now = null)
	{
		$id = (string)$id;
		if (!FaxLog::uniqueidOk($uniqueid))
			return [false, sprintf(_('Not a call id: %s'), $uniqueid)];
		$now = $now === null ? time() : (int)$now;
		$s = $this->getSettings();
		$e = FaxLog::entry($id, $uniqueid, $vars,
				   FaxMail::findFile($s['fax_dir'], $uniqueid), $now);
		$r = [true, sprintf(_('Fax of call %s logged'), $uniqueid)];
		/* junk fax control: a blocked fax is logged, not mailed */
		$why = FaxJunk::blocked($e['cid'], $s);
		if ($why !== '') {
			$e['blocked'] = true;
			$e['mail'] = $why;
			$r = [true, sprintf(_('Fax of call %s blocked: %s'), $uniqueid, $why)];
		} elseif ($id !== '') {
			$r = $this->faxToMail($id, $uniqueid, $vars, $send);
			$e['mail'] = $r[1];
			$e['mail_ok'] = (bool)$r[0];
		}
		$this->setConfig($e['uniqueid'], $e, self::FAXLOG);
		$this->purgeFaxes($now);
		return $r;
	}

	/* the caller ID of the received fax of call uniqueid added to the
	   block list (fax_block).  [ok, message] */
	public function blockCaller($uniqueid)
	{
		$e = $this->getFax($uniqueid);
		if ($e === null || $e['direction'] !== 'in')
			return [false, sprintf(_('No received fax %s'), $uniqueid)];
		$d = FaxJunk::digits($e['cid']);
		if ($d === '')
			return [false, _('The fax has no caller ID number')];
		$s = $this->getSettings();
		if (FaxJunk::listed($d, $s['fax_allow']))
			return [false, sprintf(_('%s is on the allow list'), $d)];
		$s['fax_block'] = FaxJunk::add($s['fax_block'], $d);
		$why = FaxJunk::listError($s['fax_block']);
		if ($why !== '')
			return [false, $why];
		$this->saveSettings(['fax_block' => $s['fax_block']]);
		return [true, sprintf(_('Faxes from %s are blocked from now on'), $d)];
	}

	/* the fax log: uniqueid => entry (FaxLog::entry()), the newest
	   first */
	public function getFaxes()
	{
		$all = $this->getAll(self::FAXLOG);
		$out = [];
		if (is_array($all))
			foreach ($all as $u => $e) {
				$e = FaxLog::normalize($e);
				if ($e !== null && $e['uniqueid'] === (string)$u)
					$out[$e['uniqueid']] = $e;
			}
		uasort($out, function ($a, $b) {
			return [$b['time'], $b['uniqueid']] <=> [$a['time'], $a['uniqueid']];
		});
		return $out;
	}

	/* the log entry of call uniqueid, null if there is none */
	public function getFax($uniqueid)
	{
		if (!FaxLog::uniqueidOk($uniqueid))
			return null;
		$e = FaxLog::normalize($this->getConfig((string)$uniqueid, self::FAXLOG));
		return $e !== null && $e['uniqueid'] === (string)$uniqueid ? $e : null;
	}

	/* the path of entry e's fax file, null if it has none or the file
	   is not there (only a file in fax_dir) */
	public function faxPath(array $e)
	{
		$name = FaxLog::fileName($e['file']);
		if ($name === '')
			return null;
		$p = rtrim($this->getSettings()['fax_dir'], '/') . '/' . $name;
		return is_file($p) ? $p : null;
	}

	/* the fax of call uniqueid dropped from the log, its file too.
	   [ok, message] */
	public function deleteFax($uniqueid)
	{
		$e = $this->getFax($uniqueid);
		if ($e === null)
			return [false, sprintf(_('No fax of call %s'), $uniqueid)];
		$p = $this->faxPath($e);
		if ($p !== null && !@unlink($p))
			return [false, sprintf(_('Cannot delete %s'), $p)];
		$this->setConfig($e['uniqueid'], false, self::FAXLOG);
		return [true, sprintf(_('Fax of call %s deleted'), $uniqueid)];
	}

	/* the faxes older than fax_keep_days (0: none for age) at the time
	   now dropped, and the oldest above FaxLog::MAX_ENTRIES.  The
	   number dropped */
	public function purgeFaxes($now = null)
	{
		$now = $now === null ? time() : (int)$now;
		$days = (int)$this->getSettings()['fax_keep_days'];
		$n = 0;
		foreach (FaxLog::expired($this->getFaxes(), $days, $now) as $u)
			if ($this->deleteFax($u)[0])
				$n++;
		return $n;
	}

	/* the fax of entry e as [MIME type, file name, bytes] for the
	   browser: kind pdf (FaxPdf, paper fax_paper) or tiff; null if
	   there is no file, or no PDF of it */
	public function faxDocument(array $e, $kind)
	{
		$p = $this->faxPath($e);
		if ($p === null)
			return null;
		$b = @file_get_contents($p);
		if ($b === false)
			return null;
		$base = substr(basename($p), 0, -4);
		if ($kind === 'tiff')
			return ['image/tiff', $base . '.tif', $b];
		try {
			return ['application/pdf', $base . '.pdf',
				FaxPdf::fromTiff($b, $this->getSettings()['fax_paper'])];
		} catch (\RuntimeException $x) {
			return null;
		}
	}

	/* BMO Ajax: the commands that this module answers (faxpdf,
	   faxtiff: a logged fax for a signed-in administrator) */
	public function ajaxRequest($command, &$setting)
	{
		if ($command !== 'faxpdf' && $command !== 'faxtiff')
			return false;
		$setting['authenticate'] = true;
		$setting['allowremote'] = false;
		return true;
	}

	/* BMO Ajax: no JSON answers (ajaxCustomHandler() sends the files) */
	public function ajaxHandler()
	{
		return false;
	}

	/* BMO Ajax: the fax of $_REQUEST['uniqueid'] sent to the browser,
	   a PDF to view (faxpdf) or the TIFF file to keep (faxtiff).  true
	   when it sent an answer */
	public function ajaxCustomHandler()
	{
		$cmd = isset($_REQUEST['command']) ? (string)$_REQUEST['command'] : '';
		$u = isset($_REQUEST['uniqueid']) ? (string)$_REQUEST['uniqueid'] : '';
		$e = $this->getFax($u);
		$this->sendDocument($e === null ? null :
			$this->faxDocument($e, $cmd === 'faxtiff' ? 'tiff' : 'pdf'), $cmd === 'faxtiff');
		return true;
	}

	/* document d (faxDocument(): [type, name, bytes], null for none)
	   to the browser: to view, or to keep (download) */
	public function sendDocument($d, $download)
	{
		if ($d === null) {
			self::headers(['Content-Type: text/plain; charset=utf-8'], 404);
			echo _('No such fax file'), "\n";
			return;
		}
		self::headers([
			'Content-Type: ' . $d[0],
			'Content-Length: ' . strlen($d[2]),
			'X-Content-Type-Options: nosniff',
			'Content-Disposition: ' . ($download ? 'attachment' : 'inline') .
			'; filename="' . $d[1] . '"',
		], 200);
		echo $d[2];
	}

	/* the uploaded files f ($_FILES of a multiple file input) as a list
	   of [name, bytes]; *why: '' or what went wrong */
	public static function uploads($f, &$why = null)
	{
		$why = '';
		$out = [];
		if (!is_array($f) || !isset($f['name']))
			return $out;
		foreach ((array)$f['name'] as $i => $name) {
			$e = is_array($f['error']) ? $f['error'][$i] : $f['error'];
			if ($e === UPLOAD_ERR_NO_FILE)
				continue;
			$tmp = is_array($f['tmp_name']) ? $f['tmp_name'][$i] : $f['tmp_name'];
			if ($e !== UPLOAD_ERR_OK || !is_uploaded_file($tmp)) {
				$why = sprintf(_('%s did not upload'), $name);
				return [];
			}
			$out[] = [basename((string)$name), (string)file_get_contents($tmp)];
		}
		return $out;
	}

	/* the faxes to send (the queue): id => job, the oldest first */
	public function getFaxJobs()
	{
		$all = $this->getAll(self::FAXJOBS);
		$out = [];
		if (is_array($all))
			foreach ($all as $id => $j)
				if (is_array($j) && FaxSend::idOk($id) &&
				    in_array(isset($j['state']) ? $j['state'] : '', FaxSend::STATES, true))
					$out[(string)$id] = $j + ['id' => (string)$id];
		uasort($out, function ($a, $b) {
			return [$a['created'], $a['id']] <=> [$b['created'], $b['id']];
		});
		return $out;
	}

	/* job id of the queue, null if there is none */
	public function getFaxJob($id)
	{
		$all = $this->getFaxJobs();
		return isset($all[(string)$id]) ? $all[(string)$id] : null;
	}

	/* job j kept in the queue */
	private function saveFaxJob(array $j)
	{
		$this->setConfig($j['id'], $j, self::FAXJOBS);
	}

	/* a fax to send: job in (FaxSend::normalizeJob()'s fields) with
	   the documents docs (a list of [name, bytes]) made into its fax
	   file and put in the queue, at the time now (null: time()); run
	   the runner of Ghostscript (FaxSend::make()); user: the User
	   Manager user id that sends it (UCP; 0 for none).  ['ok',
	   'message', 'errors' (field => message), 'id'] */
	public function queueFax(array $in, array $docs, $now = null, ?callable $run = null,
				 $user = 0)
	{
		$now = $now === null ? time() : (int)$now;
		$s = $this->getSettings();
		$j = FaxSend::normalizeJob($in, $s);
		$e = FaxSend::validateJob($j, count($docs));
		foreach ($docs as $d)
			if (strlen($d[1]) > FaxSend::MAX_BYTES)
				$e['docs'] = sprintf(_('%s is too big'), $d[0]);
		if ($e)
			return ['ok' => false, 'message' => implode('; ', $e), 'errors' => $e,
				'id' => ''];
		$id = FaxSend::newId($now);
		$out = rtrim($s['fax_dir'], '/') . '/' . FaxSend::fileName($id);
		$c = [
			'name' => $s['fax_cover_name'], 'address' => $s['fax_cover_address'],
			'phone' => $s['fax_cover_phone'], 'web' => $s['fax_cover_web'],
			'footer' => $s['fax_cover_footer'], 'date' => date('Y-m-d H:i', $now),
		];
		list($ok, $why, $pages) = FaxSend::make($s, $j, $docs, $c, $out, $run);
		if (!$ok)
			return ['ok' => false, 'message' => $why, 'errors' => ['docs' => $why],
				'id' => ''];
		$this->saveFaxJob($j + [
			'id' => $id, 'created' => $now, 'state' => 'queued',
			'attempts' => 0, 'next' => $now, 'started' => 0,
			'pages' => $pages, 'last' => '', 'user' => (int)$user,
		]);
		return ['ok' => true, 'errors' => [], 'id' => $id,
			'message' => sprintf(_('Fax to %s queued (%d pages)'), $j['number'], $pages)];
	}

	/* the queue run at the time now (null: time()): each job whose
	   time has come is called (the manager's Originate); a try without
	   a result after its time is a failure.  Its lines */
	public function runFaxQueue($now = null)
	{
		$now = $now === null ? time() : (int)$now;
		$s = $this->getSettings();
		$lines = [];
		$am = isset($this->FreePBX->astman) ? $this->FreePBX->astman : null;
		foreach ($this->getFaxJobs() as $id => $j) {
			if ($j['state'] === 'sending') {
				if ($j['started'] < $now - (int)$s['dialout_ring'] - FaxSend::MAX_TRY) {
					$r = $this->faxSent($id, '', 'FAILED', 0, 0, 'NO_RESULT', '', null, $now);
					$lines[] = $r[1];
				}
				continue;
			}
			if ($j['next'] > $now)
				continue;
			$j['attempts']++;
			$j['state'] = 'sending';
			$j['started'] = $now;
			$this->saveFaxJob($j);
			if (!$am || (method_exists($am, 'connected') && !$am->connected())) {
				$res = ['Response' => 'Error', 'Message' => _('the manager is not connected')];
			} else {
				$res = $am->send_request('Originate', FaxSend::originate($s, $j));
			}
			if (is_array($res) && isset($res['Response']) &&
			    strcasecmp($res['Response'], 'Success') === 0) {
				$lines[] = sprintf(_('Fax %s: calling %s (try %d)'), $id, $j['number'],
						   $j['attempts']);
				continue;
			}
			$why = is_array($res) && isset($res['Message']) ? $res['Message'] : '';
			$r = $this->faxSent($id, '', 'FAILED', 0, 0,
					    'ASTERISK_REFUSED ' . $why, '', null, $now);
			$lines[] = $r[1];
		}
		return $lines;
	}

	/* fwconsole dmodem faxsent: the result of a try of job id (the
	   call uniqueid, status, pages, rate, error, the remote fax
	   number) at the time now; a job with tries left waits for its
	   next try, else it goes into the fax log and its result by email
	   (send as FaxMail::send() has it).  [ok, message] */
	public function faxSent($id, $uniqueid, $status, $pages, $rate, $error, $remote,
				?callable $send = null, $now = null)
	{
		$now = $now === null ? time() : (int)$now;
		$j = $this->getFaxJob($id);
		if ($j === null)
			return [false, sprintf(_('No fax job %s'), $id)];
		$status = preg_replace('/[^A-Z]/', '', (string)$status);
		$j['last'] = trim($status . ' ' . $error);
		$j['result'] = ['uniqueid' => (string)$uniqueid, 'status' => $status,
				'pages' => (int)$pages, 'rate' => (int)$rate,
				'error' => (string)$error, 'remote' => (string)$remote];
		list($j, $final) = FaxSend::after($j, $status, $this->getSettings(), $now);
		if (!$final) {
			$this->saveFaxJob($j);
			return [true, sprintf(_('Fax %s to %s: %s; try %d of %d at %s'), $id,
				$j['number'], $j['last'], $j['attempts'] + 1, $j['retries'] + 1,
				date('H:i', $j['next']))];
		}
		return $this->finishFax($j, $status, $send, $now);
	}

	/* job j ended with status: into the fax log, out of the queue, and
	   its result by email when its notify setting says so.  [ok,
	   message] */
	private function finishFax(array $j, $status, ?callable $send, $now)
	{
		$s = $this->getSettings();
		$r = isset($j['result']) ? $j['result'] : [];
		$u = isset($r['uniqueid']) && FaxLog::uniqueidOk($r['uniqueid']) ?
			$r['uniqueid'] : sprintf('%d.%d', $now, crc32($j['id']) % 100000);
		$file = rtrim($s['fax_dir'], '/') . '/' . FaxSend::fileName($j['id']);
		$e = FaxLog::entry('', $u, [
			'status' => $status,
			'pages' => isset($r['pages']) ? $r['pages'] : 0,
			'rate' => isset($r['rate']) ? $r['rate'] : 0,
			'remote' => isset($r['remote']) ? $r['remote'] : '',
			'cid' => '', 'cidname' => $j['to'],
		], is_file($file) ? $file : null, $now,
			['number' => $j['number'], 'job' => $j['id'], 'attempts' => $j['attempts'],
			 'user' => isset($j['user']) ? $j['user'] : 0]);
		$msg = sprintf(_('Fax %s to %s: %s after %d tries'), $j['id'], $j['number'],
			       $status === 'SUCCESS' ? _('sent') : $j['last'], $j['attempts']);
		if (FaxSend::notifies($j, $status)) {
			$m = $this->mailResult($j, $status, $e, $file, $send);
			$e['mail'] = $m[1];
			$e['mail_ok'] = $m[0];
			$msg .= '; ' . $m[1];
		}
		$this->setConfig($u, $e, self::FAXLOG);
		$this->setConfig($j['id'], false, self::FAXJOBS);
		$this->purgeFaxes($now);
		return [true, $msg];
	}

	/* the result of job j (status, its log entry e, its fax file) to
	   the job's email address, with the PDF of the fax.  [ok, message] */
	private function mailResult(array $j, $status, array $e, $file, ?callable $send)
	{
		$s = $this->getSettings();
		$ok = $status === 'SUCCESS';
		$subject = sprintf($ok ? _('Fax to %s sent') : _('Fax to %s not sent'), $j['number']);
		$text = implode("
", [
			sprintf(_('Fax to %s (%s): %s.'), $j['number'], $j['to'],
				$ok ? _('sent') : $j['last']),
			sprintf(_('Pages: %d of %d. Tries: %d.'), $e['pages'], $j['pages'], $j['attempts']),
			sprintf(_('Queued %s.'), date('Y-m-d H:i', $j['created'])),
		]);
		$att = [];
		$b = is_file($file) ? @file_get_contents($file) : false;
		if ($b !== false)
			$att = FaxMail::attachments('fax-' . preg_replace('/[^0-9+]/', '', $j['number']),
						    $b, 'pdf', $s['fax_paper']);
		list($headers, $body, $subj) = FaxMail::build($s['fax_email_from'], $subject,
							      $text, $att);
		if ($send === null)
			$send = function ($to, $subject, $body, $headers) {
				return mail($to, $subject, $body, $headers);
			};
		$sent = (bool)$send($j['email'], $subj, $body, $headers);
		return [$sent, $sent ? sprintf(_('The result mailed to %s'), $j['email']) :
			sprintf(_('The mail system did not take the result for %s'), $j['email'])];
	}

	/* job id taken out of the queue before its try (CANCELLED in the
	   fax log).  [ok, message] */
	public function cancelFax($id, $now = null)
	{
		$j = $this->getFaxJob($id);
		if ($j === null)
			return [false, sprintf(_('No fax job %s'), $id)];
		if ($j['state'] === 'sending')
			return [false, sprintf(_('Fax %s is on a call now'), $id)];
		$j['last'] = 'CANCELLED';
		return $this->finishFax($j, 'CANCELLED', null, $now === null ? time() : (int)$now);
	}

	/* FreePBX's module name (Ucp, Userman), null if it is not there */
	private function module($name)
	{
		try {
			return isset($this->FreePBX->$name) ? $this->FreePBX->$name : null;
		} catch (\Throwable $e) {
			return null;
		}
	}

	/* the fax rights in UCP of User Manager user uid (its own, else its
	   groups'): ['enabled' (the Fax widget), 'send', 'recipients' (the
	   ids of the fax recipients whose faxes it sees: those given to it,
	   and those of its User Manager user)] */
	public function ucpFaxAccess($uid)
	{
		$out = ['enabled' => false, 'send' => false, 'recipients' => []];
		$ucp = $this->module('Ucp');
		if ($ucp === null || !method_exists($ucp, 'getCombinedSettingByID'))
			return $out;
		$out['enabled'] = (bool)$ucp->getCombinedSettingByID($uid, 'Dmodem', 'fax_enable');
		if (!$out['enabled'])
			return $out;
		$out['send'] = (bool)$ucp->getCombinedSettingByID($uid, 'Dmodem', 'fax_send');
		$ids = (array)$ucp->getCombinedSettingByID($uid, 'Dmodem', 'fax_recipients');
		$um = $this->module('Userman');
		$u = $um !== null && method_exists($um, 'getUserByID') ?
			(array)$um->getUserByID($uid) : [];
		$name = isset($u['username']) ? (string)$u['username'] : '';
		foreach ($this->getRecipients() as $id => $r)
			if (in_array($id, $ids, true) || ($name !== '' && $r['user'] === $name))
				$out['recipients'][] = (string)$id;
		return $out;
	}

	/* the faxes of the fax log that user uid sees in UCP: those
	   received for its recipients (not the blocked ones), and those
	   that it sent; the newest
	   first (uniqueid => entry) */
	public function ucpFaxes($uid)
	{
		$a = $this->ucpFaxAccess($uid);
		$out = [];
		if (!$a['enabled'])
			return $out;
		foreach ($this->getFaxes() as $u => $e)
			if (($e['direction'] === 'in' && !$e['blocked'] &&
			     in_array($e['recipient'], $a['recipients'], true)) ||
			    ($e['direction'] === 'out' && $e['user'] === (int)$uid))
				$out[$u] = $e;
		return $out;
	}

	/* the faxes of the queue that user uid sent (id => job) */
	public function ucpFaxJobs($uid)
	{
		$out = [];
		if (!$this->ucpFaxAccess($uid)['enabled'])
			return $out;
		foreach ($this->getFaxJobs() as $id => $j)
			if (isset($j['user']) && (int)$j['user'] === (int)$uid)
				$out[$id] = $j;
		return $out;
	}

	/* the UCP part of the User Manager page of a user or group (Ucp's
	   constructModuleConfigPages hook): mode user or group, the user (an
	   array, empty for a new one), action.  [[title, rawname, content]] */
	public function ucpConfigPage($mode, $user, $action)
	{
		$ucp = $this->module('Ucp');
		/* setting k of the user or group shown (null: none yet) */
		$get = function ($k) use ($ucp, $mode, $user) {
			if ($ucp === null)
				return null;
			if ($mode === 'group')
				return isset($_REQUEST['group']) && method_exists($ucp, 'getSettingByGID') ?
					$ucp->getSettingByGID($_REQUEST['group'], 'Dmodem', $k) : null;
			return isset($user['id']) && method_exists($ucp, 'getSettingByID') ?
				$ucp->getSettingByID($user['id'], 'Dmodem', $k) : null;
		};
		return [['title' => _('D-Modem Fax'), 'rawname' => 'dmodem',
			 'content' => $this->view('ucp_user', [
				'mode' => $mode,
				'enable' => $get('fax_enable'),
				'send' => $get('fax_send'),
				'chosen' => (array)$get('fax_recipients'),
				'recipients' => $this->getRecipients(),
			 ])]];
	}

	/* the form of ucpConfigPage() kept for user (or group: gid) id, from
	   the request req; a value "inherit" (users only) keeps the
	   group's */
	public function ucpSave($kind, $id, array $req)
	{
		$ucp = $this->module('Ucp');
		if ($ucp === null || !isset($req['dmodem_fax_enable']))
			return;
		$set = $kind === 'group' ? 'setSettingByGID' : 'setSettingByID';
		if (!method_exists($ucp, $set))
			return;
		$ids = array_keys($this->getRecipients());
		$chosen = isset($req['dmodem_fax_recipients']) ?
			array_values(array_intersect((array)$req['dmodem_fax_recipients'], $ids)) : [];
		foreach (['fax_enable', 'fax_send'] as $k) {
			$v = isset($req['dmodem_' . $k]) ? (string)$req['dmodem_' . $k] : 'no';
			$ucp->$set($id, 'Dmodem', $k, $v === 'inherit' && $kind === 'user' ? null : $v === 'yes');
		}
		$ucp->$set($id, 'Dmodem', 'fax_recipients', $chosen);
	}

	/* Ucp's hooks of the User Manager pages: a user or a group added
	   or changed (id, the page display, ...) */
	public function ucpAddUser($id, $display, $ucpStatus = null, $data = null)
	{
		if ($display === 'userman')
			$this->ucpSave('user', $id, $_POST);
	}

	/* see ucpAddUser() */
	public function ucpUpdateUser($id, $display, $ucpStatus = null, $data = null)
	{
		$this->ucpAddUser($id, $display, $ucpStatus, $data);
	}

	/* see ucpAddUser(): a group */
	public function ucpAddGroup($id, $display, $ucpStatus = null, $data = null)
	{
		if ($display === 'userman')
			$this->ucpSave('group', $id, $_POST);
	}

	/* see ucpAddGroup() */
	public function ucpUpdateGroup($id, $display, $ucpStatus = null, $data = null)
	{
		$this->ucpAddGroup($id, $display, $ucpStatus, $data);
	}

	/* the HTTP headers h (a list of lines) and the status code sent,
	   when no output went before them (none from the command line) */
	private static function headers(array $h, $code)
	{
		if (headers_sent())
			return;
		http_response_code($code);
		foreach ($h as $l)
			header($l);
	}

	/* the settings, profiles and fax recipients, for a backup or
	   fwconsole dmodem export: ['format', 'settings', 'profiles' (name
	   => profile), 'recipients' (id => recipient)] */
	public function exportData()
	{
		return [
			'format' => self::EXPORT_FORMAT,
			'settings' => $this->getSettings(),
			'profiles' => $this->getProfiles(),
			'recipients' => $this->getRecipients(),
			'didfax' => $this->getDidFaxes(),
		];
	}

	/* exportData()'s data back (a restore, fwconsole dmodem import):
	   the settings and each profile checked as the forms check them,
	   what passes kept; the profiles that data hasn't are dropped
	   (replace false: kept); the same for the fax recipients.  [ok,
	   settings errors, name => errors of the profiles left out (fax-<id>
	   for a recipient), names imported, recipient ids imported] */
	public function importData($data, $replace = true)
	{
		$r = ['ok' => false, 'settings' => [], 'skipped' => [], 'imported' => [],
		      'recipients' => []];
		if (!is_array($data) || !isset($data['format']) ||
		    (int)$data['format'] !== self::EXPORT_FORMAT) {
			$r['settings'] = ['format' => _('Not D-Modem settings, or of another format')];
			return $r;
		}
		if (isset($data['settings']) && is_array($data['settings'])) {
			$s = Config::normalizeSettings(array_merge(
				$this->getSettings(), $data['settings']));
			$r['settings'] = Config::validateSettings($s);
			if (!$r['settings'])
				$this->saveSettings($s);
		}
		$in = isset($data['profiles']) && is_array($data['profiles']) ?
			$data['profiles'] : [];
		$names = [];
		foreach ($in as $name => $p) {
			if (!is_array($p)) {
				$r['skipped'][(string)$name] = ['name' => _('Not a profile')];
				continue;
			}
			$p = Config::normalizeProfile(array_merge(Config::defaultProfile(),
				$p, ['name' => (string)$name]));
			$e = Config::validateProfile($p, $names);
			if ($e) {
				$r['skipped'][(string)$name] = $e;
				continue;
			}
			$names[] = $p['name'];
			$this->saveProfile($p);
			$r['imported'][] = $p['name'];
		}
		if ($replace)
			foreach (array_keys($this->getProfiles()) as $name)
				if (!in_array($name, $names, true))
					$this->deleteProfile($name);
		/* the fax recipients (an export without them: none taken,
		   none dropped) */
		if (isset($data['recipients']) && is_array($data['recipients'])) {
			$ids = [];
			$users = $this->usermanUsers();
			foreach ($data['recipients'] as $id => $rc) {
				if (!is_array($rc)) {
					$r['skipped']['fax-' . $id] = ['id' => _('Not a fax recipient')];
					continue;
				}
				$rc = Config::normalizeRecipient(array_merge(
					Config::defaultRecipient(), $rc, ['id' => (string)$id]));
				$e = Config::validateRecipient($rc, $ids, $users);
				if ($e) {
					$r['skipped']['fax-' . $id] = $e;
					continue;
				}
				$ids[] = $rc['id'];
				$this->saveRecipient($rc);
				$r['recipients'][] = $rc['id'];
			}
			if ($replace)
				foreach (array_keys($this->getRecipients()) as $id)
					if (!in_array($id, $ids, true))
						$this->deleteRecipient($id);
		}
		/* the fax detection of the inbound routes (the same) */
		if (isset($data['didfax']) && is_array($data['didfax'])) {
			$keys = [];
			foreach ($data['didfax'] as $key => $f) {
				$parts = explode('/', (string)$key, 2);
				$k = count($parts) === 2 ? Config::didKey($parts[0], $parts[1]) : null;
				$f = is_array($f) ? Config::normalizeDidFax(array_merge(
					Config::defaultDidFax(), $f)) : null;
				$e = $f === null ? ['detect' => _('Not a fax detection')] :
					Config::validateDidFax($f);
				if ($k === null)
					$e['key'] = _('Not an inbound route');
				if ($e || !$f['detect']) {
					if ($e)
						$r['skipped']['did-' . $key] = $e;
					continue;
				}
				$keys[] = $k;
				$this->saveDidFax($k, $f);
			}
			if ($replace)
				foreach (array_keys($this->getDidFaxes()) as $k)
					if (!in_array($k, $keys, true))
						$this->saveDidFax($k, Config::defaultDidFax());
		}
		$this->reload();
		$r['ok'] = !$r['settings'] && !$r['skipped'];
		return $r;
	}

	/* a modem call out: number dialled through the dial-out context,
	   the modem of profile the caller (Dialout); cid the caller ID
	   (empty: the settings').  [ok, message, errors (field => message)] */
	public function dialOut($profile, $number, $cid = '')
	{
		$e = Dialout::validate($profile, $number, $cid, $this->getProfiles());
		if ($e)
			return ['ok' => false, 'message' => implode('; ', $e), 'errors' => $e];
		$am = isset($this->FreePBX->astman) ? $this->FreePBX->astman : null;
		if (!$am || (method_exists($am, 'connected') && !$am->connected()))
			return ['ok' => false, 'errors' => [],
				'message' => _('Asterisk\'s manager interface is not connected')];
		$o = Dialout::originate($this->getSettings(), $profile, $number, $cid);
		$res = $am->send_request('Originate', $o);
		$ok = is_array($res) && isset($res['Response']) &&
			strcasecmp($res['Response'], 'Success') === 0;
		$why = is_array($res) && isset($res['Message']) ? $res['Message'] : '';
		return ['ok' => $ok, 'errors' => [], 'message' => $ok ?
			sprintf(_('Calling %s with profile %s (%s)'), $number, $profile, $o['Channel']) :
			sprintf(_('Asterisk refused the call: %s'), $why)];
	}

	/* the destination of profile (or extension) name */
	public static function destination($name)
	{
		return Dialplan::CONTEXT . ',' . $name . ',1';
	}

	/* the destination of fax recipient id */
	public static function recipientDestination($id)
	{
		return Dialplan::CONTEXT . ',' . Dialplan::RECIPIENT_PREFIX . $id . ',1';
	}

	/* fwconsole's path (FreePBX's AMPSBIN), for the dialplan's hangup
	   handler */
	public function fwconsole()
	{
		$dir = '';
		try {
			if (isset($this->FreePBX->Config))
				$dir = (string)$this->FreePBX->Config->get('AMPSBIN');
		} catch (\Throwable $e) {
			$dir = '';
		}
		if (!preg_match('#^/[A-Za-z0-9/._-]{0,200}$#', $dir))
			$dir = '/usr/sbin';
		return rtrim($dir, '/') . '/fwconsole';
	}

	/* the dialplan: app-dmodem's extensions, the context of each */
	public function dialplan()
	{
		$s = $this->getSettings();
		$c = [Dialplan::CONTEXT => Dialplan::build($s, $this->getProfiles(),
							   $this->getRecipients())];
		/* (the fax log's hangup handler: always) */
		$c[Dialplan::MAIL_CONTEXT] = ['s' => Dialplan::mailHandler(
			$this->fwconsole())];
		$c[Dialplan::FAXOUT_CONTEXT] = Dialplan::faxOut($s, $this->fwconsole());
		$c[Dialplan::SENT_CONTEXT] = ['s' => Dialplan::sentHandler(
			$this->fwconsole())];
		return $c;
	}

	/* what uses destination dest (FreePBX's other modules), empty if
	   nothing or unknown */
	private function destinationUsers($dest)
	{
		if (!function_exists('framework_check_destination_usage'))
			return [];
		/* (a list: FreePBX's Destinations goes through it) */
		$u = framework_check_destination_usage([$dest]);
		return is_array($u) ? $u : [];
	}

	/* the form posted for page display=dmodem: a profile saved or
	   deleted, the settings saved */
	public function doConfigPageInit($page)
	{
		$req = $_REQUEST;
		if ($page === 'did') {
			$this->didPageInit($req);
			return;
		}
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
		if ($action === 'delete_recipient' && isset($req['recipient'])) {
			$id = (string)$req['recipient'];
			if ($this->getRecipient($id) === null)
				return;
			if ($this->destinationUsers(self::recipientDestination($id))) {
				$this->message = ['danger', sprintf(_('Fax recipient %s is the destination of a route or another object: point those elsewhere first'), $id)];
				$_REQUEST['view'] = 'recipient';
				return;
			}
			$this->deleteRecipient($id);
			$this->reload();
			$this->message = ['success', sprintf(_('Fax recipient %s deleted'), $id)];
			$_REQUEST['view'] = 'recipients';
			return;
		}
		if ($action === 'send_fax') {
			$docs = self::uploads(isset($_FILES['docs']) ? $_FILES['docs'] : null, $why);
			$r = $why !== '' ? ['ok' => false, 'message' => $why,
					    'errors' => ['docs' => $why]] :
				$this->queueFax($req, $docs);
			$this->errors = $r['errors'];
			$this->posted = FaxSend::normalizeJob($req, $this->getSettings());
			if ($r['ok']) {
				$this->posted = null;
				$this->runFaxQueue();
			}
			$this->message = [$r['ok'] ? 'success' : 'danger', $r['message']];
			$_REQUEST['view'] = 'send';
			return;
		}
		if ($action === 'cancel_fax' && isset($req['job'])) {
			$r = $this->cancelFax((string)$req['job']);
			$this->message = [$r[0] ? 'success' : 'danger', $r[1]];
			$_REQUEST['view'] = 'send';
			return;
		}
		if ($action === 'block_caller' && isset($req['uniqueid'])) {
			$r = $this->blockCaller((string)$req['uniqueid']);
			$this->message = [$r[0] ? 'success' : 'danger', $r[1]];
			$_REQUEST['view'] = 'faxes';
			return;
		}
		if ($action === 'delete_fax' && isset($req['uniqueid'])) {
			$r = $this->deleteFax((string)$req['uniqueid']);
			$this->message = [$r[0] ? 'success' : 'danger', $r[1]];
			$_REQUEST['view'] = 'faxes';
			return;
		}
		if ($action === 'save_recipient') {
			$r = Config::normalizeRecipient($req);
			$old = isset($req['original']) ? (string)$req['original'] : '';
			if ($old !== '') {
				/* a recipient keeps its id: it is its destination */
				$r['id'] = $old;
				$taken = [];
			} else {
				$taken = array_keys($this->getRecipients());
			}
			$this->errors = Config::validateRecipient($r, $taken,
								  $this->usermanUsers());
			if ($this->errors) {
				$this->posted = $r;
				$_REQUEST['view'] = 'recipient';
				return;
			}
			$this->saveRecipient($r);
			$this->reload();
			$this->message = ['success', sprintf(_('Fax recipient %s saved'), $r['id'])];
			$_REQUEST['view'] = 'recipients';
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
		if ($action === 'dialout') {
			$num = isset($req['number']) ? trim((string)$req['number']) : '';
			$prof = isset($req['profile']) ? (string)$req['profile'] : '';
			$cid = isset($req['callerid']) ? trim((string)$req['callerid']) : '';
			$r = $this->dialOut($prof, $num, $cid);
			$this->errors = $r['errors'];
			$this->posted = ['profile' => $prof, 'number' => $num, 'callerid' => $cid];
			$this->message = [$r['ok'] ? 'success' : 'danger', $r['message']];
			$_REQUEST['view'] = 'dialout';
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
		if ($view !== 'form' && $view !== 'settings' && $view !== 'recipient')
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
		if ($view === 'recipient' && !empty($request['recipient']))
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
		case 'recipients':
			$body = $this->view('recipients', ['recipients' => $this->getRecipients(),
				'settings' => $settings]);
			break;
		case 'send':
			$body = $this->view('send', ['job' => $this->posted !== null ?
					$this->posted : FaxSend::defaultJob($settings),
				'jobs' => $this->getFaxJobs(), 'settings' => $settings,
				'errors' => $this->errors]);
			break;
		case 'faxes':
			$body = $this->view('faxes', ['faxes' => $this->getFaxes(),
				'recipients' => $this->getRecipients(),
				'settings' => $settings]);
			break;
		case 'recipient':
			$id = isset($_REQUEST['recipient']) ? (string)$_REQUEST['recipient'] : '';
			$r = $this->posted !== null ? $this->posted :
				($id !== '' ? $this->getRecipient($id) : null);
			$editing = $id !== '' && $this->getRecipient($id) !== null;
			if ($r === null)
				$r = Config::defaultRecipient();
			$body = $this->view('recipient', ['recipient' => $r,
				'editing' => $editing, 'users' => $this->usermanUsers(),
				'errors' => $this->errors]);
			break;
		case 'settings':
			$s = $this->posted !== null ? $this->posted : $settings;
			$body = $this->view('settings', ['s' => $s,
				'errors' => $this->errors]);
			break;
		case 'check':
			$body = $this->view('check', ['check' => $this->check($settings)]);
			break;
		case 'dialout':
			$body = $this->view('dialout', ['profiles' => $this->getProfiles(),
				'call' => $this->posted !== null ? $this->posted :
					['profile' => '', 'number' => '', 'callerid' => ''],
				'errors' => $this->errors]);
			break;
		case 'dialplan':
			$body = $this->view('dialplan', [
				'conf' => Config::render($settings, $this->getProfiles()),
				'dialplan' => implode("\n", array_map(function ($name, $x) {
					return Dialplan::render($x, $name);
				}, array_keys($this->dialplan()), $this->dialplan()))]);
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
		foreach ($this->dialplan() as $name => $context)
			foreach ($context as $exten => $steps)
				foreach ($steps as $st)
					$ext->add($name, $exten, '',
						  new \ext_dmodem_app($st[0], $st[1]));
		/* the fax detection of the inbound routes, in Core's routes
		   (its hook runs first, at 480) before their caller ID step */
		$s = $this->getSettings();
		foreach ($this->getDidFaxes() as $key => $f) {
			$parts = explode('/', $key, 2);
			$exten = Dialplan::didExten($parts[0], isset($parts[1]) ? $parts[1] : '');
			foreach (Dialplan::DID_CONTEXTS as $ctx) {
				if (!isset($ext->_exts[$ctx][' ' . $exten . ' ']))
					continue;
				foreach (Dialplan::didFax($s, $f) as $st)
					$ext->splice($ctx, $exten, Dialplan::DID_HOOK,
						     new \ext_dmodem_app($st[0], $st[1]));
			}
		}
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
		foreach ($this->getRecipients() as $id => $r)
			$d[] = [
				'destination' => self::recipientDestination($id),
				'description' => sprintf(_('Fax to %s'), $r['name']),
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
		$pre = Dialplan::RECIPIENT_PREFIX;
		if (strpos($name, $pre) === 0) {
			$id = substr($name, strlen($pre));
			$r = $this->getRecipient($id);
			if ($r !== null)
				return ['description' => sprintf(_('D-Modem: fax to %s'), $r['name']),
					'edit_url' => 'config.php?display=dmodem&view=recipient&recipient=' .
						urlencode($id)];
		}
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

	/* what the engine's fax machine can do, as lines of text: app_dmodem
	   tells it (dmodem fax show capabilities), dmodem-ari's are fixed
	   (its T.38 as the settings have it); empty if the engine is not
	   there */
	public function faxCapabilities(array $settings, $engine_ok)
	{
		if (!$engine_ok)
			return '';
		if ($settings['engine'] !== 'ari') {
			$out = $this->cli('dmodem fax show capabilities');
			return $out === null ? '' : trim($out);
		}
		return implode("\n", [
			'Technology:   dmodem-ari (libdmodem T.30)',
			'Directions:   send, receive',
			'Transports:   audio (G.711)' . ($settings['ari_t38'] ?
				', T.38 (versions 0 to 3, on the SIP side)' : ''),
			'Modems:       V.17, V.29, V.27ter, 2400 to 14400 bit/s',
			'ECM:          yes',
			'Coding:       MH, MR, MMR, T.85 (with ECM)',
			'Files:        TIFF (several in one fax)',
		]);
	}

	/* the check page's findings: the engine's state, and each PJSIP
	   endpoint's settings and what they mean */
	public function check(array $settings)
	{
		$c = ['manager' => true, 'engine' => $settings['engine'],
		      'engine_ok' => null, 'endpoints' => [], 'fax' => ''];
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
		$c['fax'] = $this->faxCapabilities($settings, $c['engine_ok']);
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
