<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Neo-Desktop
//
// ami.php - Asterisk's manager interface for the end-to-end tests, in
// place of FreePBX's php-asmanager: connected(), send_request() (the
// action's response as key => value, Output lines as a list) and
// Command().  The module talks to a real Asterisk through it.

class TestAmi
{
	private $sock = null;
	private $id = 0;

	/* logged in to 127.0.0.1:port as user/secret, or not connected */
	public function __construct($port, $user, $secret)
	{
		$this->sock = @fsockopen('127.0.0.1', (int)$port, $errno, $err, 5);
		if (!$this->sock)
			return;
		stream_set_timeout($this->sock, 15);
		fgets($this->sock);	/* the banner */
		$r = $this->send_request('Login', ['Username' => $user, 'Secret' => $secret]);
		if (!isset($r['Response']) || $r['Response'] !== 'Success')
			$this->sock = null;
	}

	/* logged in */
	public function connected()
	{
		return $this->sock !== null;
	}

	/* the next message (lines up to an empty one) as key => value; a key
	   seen again (Output) a list */
	private function message()
	{
		$m = [];
		while (($l = fgets($this->sock)) !== false) {
			$l = rtrim($l, "\r\n");
			if ($l === '')
				break;
			$c = strpos($l, ':');
			if ($c === false)
				continue;
			$k = substr($l, 0, $c);
			$v = ltrim(substr($l, $c + 1));
			if (!isset($m[$k]))
				$m[$k] = $v;
			elseif (is_array($m[$k]))
				$m[$k][] = $v;
			else
				$m[$k] = [$m[$k], $v];
		}
		return $m;
	}

	/* action with its fields; its response (events in between skipped) */
	public function send_request($action, $fields = [])
	{
		if (!$this->sock)
			return false;
		$id = 'dmodem-test-' . (++$this->id);
		$req = "Action: $action\r\nActionID: $id\r\n";
		foreach ($fields as $k => $v)
			$req .= "$k: $v\r\n";
		fwrite($this->sock, $req . "\r\n");
		for ($n = 0; $n < 1000; $n++) {
			$m = $this->message();
			if (!$m && feof($this->sock))
				return false;
			if (isset($m['ActionID']) && $m['ActionID'] === $id && isset($m['Response'])) {
				if (isset($m['Output']) && !is_array($m['Output']))
					$m['Output'] = [$m['Output']];
				return $m;
			}
		}
		return false;
	}

	/* CLI command cmd through the manager */
	public function Command($cmd)
	{
		return $this->send_request('Command', ['Command' => $cmd]);
	}
}
