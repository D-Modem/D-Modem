<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// lib/FaxSend.php - a fax that D-Modem sends: the job (the number, the
// cover page, the tries, the email of the result), its fax file (the
// documents and the cover page made into fax pages by Ghostscript), the
// manager's Originate action of a try, and the job's next state after a
// try.  The queue is in Dmodem.class.php (queueFax(), runFaxQueue(),
// faxSent()); the dialplan of a try is in Dialplan.php (faxOut()).

namespace FreePBX\modules\Dmodem;

class FaxSend
{
	/* when the result of a job goes by email */
	const NOTIFY = ['never', 'always', 'success', 'failure'];
	/* the states of a job in the queue */
	const STATES = ['queued', 'sending', 'retry'];
	/* the most documents in a job, and the most bytes of each */
	const MAX_DOCS = 10;
	const MAX_BYTES = 20971520;
	/* the fax pages: resolution (fine) and size in pixels for each
	   paper (1728 dots a line, as T.4 has it) */
	const RES = '204x196';
	const PAGE = ['letter' => '1728x2156', 'a4' => '1728x2292'];
	/* the seconds after the ring time that a try may take, after which
	   a try without a result is a failure */
	const MAX_TRY = 3600;

	/* a new job id: letters and digits only (the dialplan filters it
	   so), not a call's uniqueid */
	public static function newId($now)
	{
		return 'j' . (int)$now . bin2hex(random_bytes(3));
	}

	/* true if id is a job id */
	public static function idOk($id)
	{
		return preg_match('/^j[0-9]{1,12}[0-9a-f]{6}$/', (string)$id) === 1;
	}

	/* the fax file's name of job id, in fax_dir */
	public static function fileName($id)
	{
		return 'out-' . $id . '.tif';
	}

	/* a new job's fields, the settings' defaults (s) for what the form
	   leaves out */
	public static function defaultJob(array $s)
	{
		return [
			'number' => '',
			'cover' => false,
			'to' => '',
			'from' => '',
			'message' => '',
			'retries' => (int)$s['fax_retries'],
			'notify' => $s['fax_notify'],
			'email' => $s['fax_notify_email'],
		];
	}

	/* a job from a form or the command line (in) over defaultJob() */
	public static function normalizeJob(array $in, array $s)
	{
		$j = self::defaultJob($s);
		foreach ($j as $k => $def) {
			if (!array_key_exists($k, $in)) {
				if (is_bool($def))
					$j[$k] = false;
				continue;
			}
			$v = $in[$k];
			if (is_bool($def))
				$j[$k] = in_array(strtolower(trim((string)$v)),
						  ['1', 'yes', 'on', 'true'], true);
			elseif (is_int($def))
				$j[$k] = is_numeric($v) ? (int)$v : -1;
			else
				$j[$k] = trim((string)$v);
		}
		$j['message'] = str_replace("\r\n", "\n", $j['message']);
		return $j;
	}

	/* what is wrong with job j, field => message; ndocs: the number of
	   documents */
	public static function validateJob(array $j, $ndocs)
	{
		$e = [];
		if (!preg_match('/^[0-9*#+]{1,40}$/', $j['number']))
			$e['number'] = _('The fax number: digits, *, # and +');
		if ($ndocs < 1 && !$j['cover'])
			$e['docs'] = _('A document, or a cover page');
		if ($ndocs > self::MAX_DOCS)
			$e['docs'] = sprintf(_('%d documents at most'), self::MAX_DOCS);
		foreach (['to', 'from'] as $k)
			if (preg_match('/[\x00-\x1f]/', $j[$k]) || strlen($j[$k]) > 100)
				$e[$k] = _('One line of 100 characters at most');
		if (strpos($j['message'], "\0") !== false || strlen($j['message']) > 3000)
			$e['message'] = _('3000 characters at most');
		if (!is_int($j['retries']) || $j['retries'] < 0 || $j['retries'] > 3)
			$e['retries'] = _('From 0 to 3');
		if (!in_array($j['notify'], self::NOTIFY, true))
			$e['notify'] = _('Never, always, on success or on failure');
		elseif ($j['notify'] !== 'never' && !FaxMail::addressOk($j['email']))
			$e['email'] = _('An email address for the result');
		return $e;
	}

	/* the kind of document bytes b: pdf, tiff, or null for neither */
	public static function kind($b)
	{
		if (strncmp($b, '%PDF-', 5) === 0)
			return 'pdf';
		if (strncmp($b, "II*\0", 4) === 0 || strncmp($b, "MM\0*", 4) === 0)
			return 'tiff';
		return null;
	}

	/* the command (a list: no shell) of Ghostscript gs that makes the
	   PDF files pdfs (absolute paths: none starts with -) into the fax
	   pages of out on paper */
	public static function gsCommand($gs, array $pdfs, $out, $paper)
	{
		return array_merge([$gs, '-q', '-dNOPAUSE', '-dBATCH', '-dSAFER',
			'-sDEVICE=tiffg4', '-r' . self::RES,
			'-g' . (isset(self::PAGE[$paper]) ? self::PAGE[$paper] : self::PAGE['letter']),
			'-dPDFFitPage', '-dFIXEDMEDIA', '-sOutputFile=' . $out], $pdfs);
	}

	/* command cmd (a list) run without a shell: [exit code, output] */
	public static function exec(array $cmd)
	{
		$p = @proc_open($cmd, [0 => ['file', '/dev/null', 'r'],
				       1 => ['pipe', 'w'], 2 => ['redirect', 1]], $pipes);
		if (!is_resource($p))
			return [127, 'cannot run ' . $cmd[0]];
		$out = stream_get_contents($pipes[1]);
		fclose($pipes[1]);
		return [proc_close($p), (string)$out];
	}

	/* the fax file out of job j: the cover page (c: its company lines
	   and the date) and the documents docs (a list of [name, bytes],
	   PDF or TIFF) as fax pages, with settings s; run: the runner of
	   the commands (exec() if null).  [ok, message, pages] */
	public static function make(array $s, array $j, array $docs, array $c, $out,
				    ?callable $run = null)
	{
		if ($run === null)
			$run = [self::class, 'exec'];
		$tmp = $out . '.d';
		if (!is_dir($tmp) && !@mkdir($tmp, 0770, true))
			return [false, sprintf(_('Cannot make %s'), $tmp), 0];
		$pdfs = [];
		$r = null;
		foreach ($docs as $i => $d) {
			list($name, $b) = $d;
			$kind = self::kind($b);
			if ($kind === 'tiff') {
				try {
					$b = FaxPdf::fromTiff($b, $s['fax_paper']);
				} catch (\RuntimeException $x) {
					$r = [false, sprintf(_('%s: not a TIFF file that D-Modem reads (%s)'),
							     $name, $x->getMessage()), 0];
					break;
				}
			} elseif ($kind === null) {
				$r = [false, sprintf(_('%s is not a PDF or TIFF file'), $name), 0];
				break;
			}
			$f = sprintf('%s/doc-%d.pdf', $tmp, $i);
			file_put_contents($f, $b);
			$pdfs[] = $f;
		}
		$pages = 0;
		if ($r === null && $pdfs) {
			/* the documents' pages first: the cover page counts them */
			$r = self::convert($s, $pdfs, "$tmp/docs.tif", $run, $pages);
			self::remove("$tmp/docs.tif");
		}
		if ($r === null || $r[0]) {
			if ($j['cover']) {
				$f = "$tmp/cover.pdf";
				file_put_contents($f, FaxCover::pdf($c + [
					'to' => $j['to'], 'number' => $j['number'],
					'from' => $j['from'], 'message' => $j['message'],
					'pages' => $pages + 1], $s['fax_paper']));
				array_unshift($pdfs, $f);
			}
			$all = 0;
			$r = self::convert($s, $pdfs, $out, $run, $all);
			$pages = $all;
		}
		foreach (glob("$tmp/*") as $f)
			self::remove($f);
		if (is_dir($tmp))
			rmdir($tmp);
		if (!$r[0])
			self::remove($out);
		return [$r[0], $r[1], $r[0] ? $pages : 0];
	}

	/* file f deleted if it is there */
	private static function remove($f)
	{
		if (is_file($f))
			unlink($f);
	}

	/* the PDF files pdfs made into the fax file out (run the runner);
	   *pages its pages.  [ok, message] */
	private static function convert(array $s, array $pdfs, $out, callable $run, &$pages)
	{
		$pages = 0;
		list($code, $text) = $run(self::gsCommand($s['fax_ghostscript'], $pdfs, $out,
							 $s['fax_paper']));
		$b = is_file($out) ? (string)@file_get_contents($out) : '';
		if ($code !== 0 || self::kind($b) !== 'tiff')
			return [false, sprintf(_('Ghostscript (%s) did not make the fax pages: %s'),
					       $s['fax_ghostscript'],
					       trim(substr((string)$text, 0, 300)))];
		try {
			$pages = count(FaxPdf::pages($b));
		} catch (\RuntimeException $x) {
			return [false, sprintf(_('The fax pages are not readable: %s'), $x->getMessage())];
		}
		return [true, sprintf(_('%d fax pages'), $pages)];
	}

	/* the manager's Originate action for a try of job j (settings s):
	   the number through the dial-out context; when the far end
	   answers, the call runs Dialplan::FAXOUT_CONTEXT (s), else its
	   failed extension */
	public static function originate(array $s, array $j)
	{
		$o = [
			'Channel' => 'Local/' . $j['number'] . '@' . $s['dialout_context'],
			'Context' => Dialplan::FAXOUT_CONTEXT,
			'Exten' => 's',
			'Priority' => '1',
			'Timeout' => (string)((int)$s['dialout_ring'] * 1000),
			'Async' => 'true',
			'Variable' => 'DMODEMFAXJOB=' . $j['id'],
		];
		if ($s['dialout_callerid'] !== '')
			$o['CallerID'] = $s['dialout_callerid'];
		return $o;
	}

	/* job j after the try with status (SUCCESS, FAILED, NOANSWER ...)
	   at the time now (s the settings): [job, final].  Not final: the
	   job waits (retry) for its next try */
	public static function after(array $j, $status, array $s, $now)
	{
		$j['state'] = 'retry';
		$final = $status === 'SUCCESS' || $j['attempts'] > $j['retries'];
		if (!$final)
			$j['next'] = (int)$now + (int)$s['fax_retry_delay'] * 60;
		return [$j, $final];
	}

	/* true if the result of a job that ended with status goes by email
	   (its notify setting) */
	public static function notifies(array $j, $status)
	{
		$ok = $status === 'SUCCESS';
		return $j['notify'] === 'always' ||
			($j['notify'] === 'success' && $ok) ||
			($j['notify'] === 'failure' && !$ok && $status !== 'CANCELLED');
	}
}
