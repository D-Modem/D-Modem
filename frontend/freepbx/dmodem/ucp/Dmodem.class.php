<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// ucp/Dmodem.class.php - the Fax widget of the User Control Panel (UCP):
// the faxes received for the user's fax recipients and the faxes that the
// user sent, each to view as PDF, download, forward or delete; a form to
// send a fax; the faxes that wait, to cancel; a browser notification
// when a fax comes.  The rights and the data come from the module's BMO
// (Dmodem::ucpFaxAccess(), ucpFaxes(), ucpFaxJobs(), queueFax()); User
// Manager gives the rights (Dmodem::ucpConfigPage()).

namespace UCP\Modules;

use \UCP\Modules as Modules;

#[\AllowDynamicProperties]
class Dmodem extends Modules
{
	protected $module = 'Dmodem';
	private $uid = 0;
	/* the runner of the Ghostscript command (FaxSend::make()); null:
	   FaxSend::exec().  The tests give a stand-in */
	public $run = null;

	/* the module for the signed-in user */
	public function __construct($Modules)
	{
		$this->Modules = $Modules;
		$user = $this->UCP->User->getUser();
		$this->uid = is_array($user) && isset($user['id']) ? (int)$user['id'] : 0;
	}

	/* the module's BMO object */
	private function bmo()
	{
		return $this->UCP->FreePBX->Dmodem;
	}

	/* the user's fax rights (Dmodem::ucpFaxAccess()) */
	private function access()
	{
		return $this->uid ? $this->bmo()->ucpFaxAccess($this->uid) :
			['enabled' => false, 'send' => false, 'recipients' => []];
	}

	/* the widgets that the user can add: one Fax widget, if it is on */
	public function getWidgetList()
	{
		if (!$this->access()['enabled'])
			return [];
		return [
			'rawname' => 'dmodem',
			'display' => _('Fax'),
			'icon' => 'fa fa-fax',
			'list' => [
				'fax' => [
					'display' => _('Fax'),
					'description' => _('Your received and sent faxes, and send a fax'),
					'defaultsize' => ['height' => 7, 'width' => 6],
					'minsize' => ['height' => 5, 'width' => 4],
				],
			],
		];
	}

	/* the widget id's content: ['title', 'html'] */
	public function getWidgetDisplay($id, $uuid = null)
	{
		return ['title' => _('Fax'), 'html' => $this->widgetHtml()];
	}

	/* the widget's HTML: the folders, the send form */
	public function widgetHtml()
	{
		$a = $this->access();
		if (!$a['enabled'])
			return '<div class="alert alert-info">' . _('Fax is not on for you.') . '</div>';
		$faxes = $this->bmo()->ucpFaxes($this->uid);
		$s = $this->bmo()->getSettings();
		$folders = ['inbox' => [], 'sent' => [], 'failed' => []];
		foreach ($faxes as $u => $e)
			$folders[$e['direction'] === 'in' ? 'inbox' :
				 ($e['status'] === 'SUCCESS' ? 'sent' : 'failed')][$u] = $e;
		return $this->load_view(__DIR__ . '/views/widget.php', [
			'folders' => $folders,
			'jobs' => $this->bmo()->ucpFaxJobs($this->uid),
			'send' => $a['send'],
			'job' => \FreePBX\modules\Dmodem\FaxSend::defaultJob($s),
		]);
	}

	/* the poll of the open widgets: the newest received fax's time and
	   uniqueid, for the browser notification */
	public function poll($data = null)
	{
		$latest = ['time' => 0, 'uniqueid' => '', 'from' => ''];
		foreach ($this->bmo()->ucpFaxes($this->uid) as $u => $e) {
			if ($e['direction'] !== 'in')
				continue;
			$latest = ['time' => $e['time'], 'uniqueid' => (string)$u,
				   'from' => trim($e['cidname'] . ' ' . $e['cid'])];
			break;
		}
		return ['latest' => $latest];
	}

	/* the Ajax commands that the user may send */
	public function ajaxRequest($command, $settings)
	{
		$a = $this->access();
		if (!$a['enabled'])
			return false;
		switch ($command) {
		case 'faxlist':
		case 'faxpdf':
		case 'faxtiff':
		case 'faxdelete':
			return true;
		case 'faxsend':
		case 'faxforward':
		case 'faxcancel':
			return $a['send'];
		}
		return false;
	}

	/* the fax uniqueid u of the user's faxes, null if it is not one */
	private function fax($u)
	{
		$all = $this->bmo()->ucpFaxes($this->uid);
		return isset($all[(string)$u]) ? $all[(string)$u] : null;
	}

	/* faxpdf and faxtiff: the file to the browser (true: sent) */
	public function ajaxCustomHandler()
	{
		$cmd = isset($_REQUEST['command']) ? (string)$_REQUEST['command'] : '';
		if ($cmd !== 'faxpdf' && $cmd !== 'faxtiff')
			return false;
		$e = $this->fax(isset($_REQUEST['uniqueid']) ? $_REQUEST['uniqueid'] : '');
		$d = $e === null ? null :
			$this->bmo()->faxDocument($e, $cmd === 'faxtiff' ? 'tiff' : 'pdf');
		$this->bmo()->sendDocument($d, $cmd === 'faxtiff');
		return true;
	}

	/* the other commands: ['status', 'message', 'html' (the widget)] */
	public function ajaxHandler()
	{
		$cmd = isset($_REQUEST['command']) ? (string)$_REQUEST['command'] : '';
		$r = [true, ''];
		switch ($cmd) {
		case 'faxlist':
			break;
		case 'faxdelete':
			$u = isset($_POST['uniqueid']) ? (string)$_POST['uniqueid'] : '';
			$r = $this->fax($u) === null ? [false, _('No such fax')] :
				$this->bmo()->deleteFax($u);
			break;
		case 'faxsend':
			$docs = \FreePBX\modules\Dmodem::uploads(isset($_FILES['docs']) ? $_FILES['docs'] : null, $why);
			if ($why !== '') {
				$r = [false, $why];
				break;
			}
			$q = $this->bmo()->queueFax($_POST, $docs, null, $this->run, $this->uid);
			$r = [$q['ok'], $q['message']];
			break;
		case 'faxforward':
			$e = $this->fax(isset($_POST['uniqueid']) ? $_POST['uniqueid'] : '');
			$p = $e === null ? null : $this->bmo()->faxPath($e);
			if ($p === null) {
				$r = [false, _('No such fax')];
				break;
			}
			$q = $this->bmo()->queueFax(['number' => isset($_POST['number']) ? $_POST['number'] : ''],
				[[basename($p), (string)file_get_contents($p)]], null, $this->run, $this->uid);
			$r = [$q['ok'], $q['message']];
			break;
		case 'faxcancel':
			$id = isset($_POST['job']) ? (string)$_POST['job'] : '';
			$jobs = $this->bmo()->ucpFaxJobs($this->uid);
			$r = isset($jobs[$id]) ? $this->bmo()->cancelFax($id) : [false, _('No such fax')];
			break;
		default:
			return false;
		}
		if ($r[0] && ($cmd === 'faxsend' || $cmd === 'faxforward'))
			$this->bmo()->runFaxQueue();
		return ['status' => (bool)$r[0], 'message' => $r[1], 'html' => $this->widgetHtml()];
	}
}
