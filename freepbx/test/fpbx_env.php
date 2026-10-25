<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Neo-Desktop
//
// fpbx_env.php - what the module uses of FreePBX, for the tests: the
// kvstore (DB_Helper's rules: false deletes, scalars come back as
// strings, arrays as JSON), BMO, the manager (canned CLI output),
// WriteConfig, needreload(), the destination usage, and the dialplan's
// extensions class.  With FREEPBX_FRAMEWORK=<a checkout of FreePBX's
// framework> its own extensions.class.php is used instead of ours.

error_reporting(E_ALL);
set_error_handler(function ($no, $str, $file, $line) {
	throw new ErrorException($str, 0, $no, $file, $line);
});

define('FREEPBX_IS_AUTH', true);

if (!function_exists('_')) {
	/* gettext's _() */
	function _($s)
	{
		return $s;
	}
}

interface BMO
{
	public function install();
	public function uninstall();
}

/* DB_Helper's kvstore, in memory: a table per class, rows (id, key) */
class FreePBX_Helpers
{
	public static $kv = [];

	/* this class's table */
	private function &table()
	{
		$t = get_class($this);
		if (!isset(self::$kv[$t]))
			self::$kv[$t] = [];
		return self::$kv[$t];
	}

	/* key of group id, false if none */
	public function getConfig($key, $id = 'noid')
	{
		$t = &$this->table();
		if (!isset($t[$id][$key]))
			return false;
		list($type, $val) = $t[$id][$key];
		return $type === 'json-arr' ? json_decode($val, true) : $val;
	}

	/* key of group id set to val; false deletes it */
	public function setConfig($key, $val = false, $id = 'noid')
	{
		$t = &$this->table();
		if ($val === false) {
			unset($t[$id][$key]);
			return true;
		}
		if (is_array($val))
			$t[$id][$key] = ['json-arr', json_encode($val)];
		else
			/* as PDO stores and gives it back: a string ("1" for true) */
			$t[$id][$key] = [null, (string)$val];
		return true;
	}

	/* group id, key => value in key order */
	public function getAll($id = 'noid')
	{
		$t = &$this->table();
		$out = [];
		if (isset($t[$id])) {
			$keys = array_keys($t[$id]);
			sort($keys, SORT_STRING);
			foreach ($keys as $k)
				$out[$k] = $this->getConfig($k, $id);
		}
		return $out;
	}

	/* group id gone */
	public function delById($id)
	{
		$t = &$this->table();
		unset($t[$id]);
	}
}

/* the manager: CLI command => output */
class FakeAstman
{
	public $out = [];
	public $up = true;

	/* the connection is up */
	public function connected()
	{
		return $this->up;
	}

	/* cmd's output, as php-asmanager hands Command back */
	public function Command($cmd)
	{
		return ['Response' => 'Follows', 'data' =>
			"Privilege: Command\n" .
			(isset($this->out[$cmd]) ? $this->out[$cmd] : "No such command '$cmd'\n")];
	}
}

/* FreePBX's object: the manager, and the files WriteConfig was given */
class FakeFreePBX
{
	public $astman;
	public $written = [];

	/* a manager with no output yet */
	public function __construct()
	{
		$this->astman = new FakeAstman();
	}

	/* files filename => contents written */
	public function WriteConfig($conf)
	{
		foreach ($conf as $f => $c)
			$this->written[$f] = $c;
	}
}

/* FreePBX::Dmodem() for functions.inc.php */
class FreePBX
{
	public static $modules = [];

	/* FreePBX::<Module>() */
	public static function __callStatic($name, $args)
	{
		return self::$modules[$name];
	}
}

$GLOBALS['needreload'] = 0;
/* the Apply Config button shown */
function needreload()
{
	$GLOBALS['needreload']++;
}

$GLOBALS['dest_usage'] = [];
/* what uses destination dest: the test says */
function framework_check_destination_usage($dest = true, $module_hash = false)
{
	return isset($GLOBALS['dest_usage'][$dest]) ? $GLOBALS['dest_usage'][$dest] : [];
}

$fw = getenv('FREEPBX_FRAMEWORK');
$fwext = $fw ? $fw . '/amp_conf/htdocs/admin/libraries/extensions.class.php' : '';
if ($fwext !== '' && is_file($fwext)) {
	require_once $fwext;
	define('DMODEM_TEST_REAL_EXTENSIONS', true);
} else {
	define('DMODEM_TEST_REAL_EXTENSIONS', false);

	/* FreePBX's extension: a dialplan step */
	class extension
	{
		public $data;

		/* the step's data */
		public function __construct($data = '')
		{
			$this->data = $data;
		}

		/* the step's text */
		public function output()
		{
			return $this->data;
		}
	}

	/* FreePBX's extensions: add() and the dialplan's text, as
	   extensions.class.php writes it */
	class extensions
	{
		public $_exts = [];

		/* command added to extension of section (1 first, then n) */
		public function add($section, $extension, $tag, $command)
		{
			$extension = ' ' . $extension . ' ';
			$basetag = empty($this->_exts[$section][$extension]) ? '1' : 'n';
			$this->_exts[$section][$extension][] = ['basetag' => $basetag,
				'tag' => $tag, 'cmd' => $command];
		}

		/* the dialplan's text */
		public function generateConf()
		{
			$o = '';
			foreach ($this->_exts as $section => $exts) {
				$o .= "[$section]\n";
				$o .= "include => {$section}-custom\n";
				foreach ($exts as $extension => $steps) {
					foreach ($steps as $st)
						$o .= 'exten => ' . trim($extension) . ',' .
							$st['basetag'] .
							($st['tag'] ? '(' . $st['tag'] . ')' : '') .
							',' . $st['cmd']->output() . "\n";
					$o .= "\n";
				}
				$o .= ";--== end of [$section] ==--;\n\n\n";
			}
			return $o;
		}
	}
}

require_once __DIR__ . '/../dmodem/Dmodem.class.php';
require_once __DIR__ . '/../dmodem/functions.inc.php';

/* a new module object on a fresh store */
function dmodem_new()
{
	FreePBX_Helpers::$kv = [];
	$fp = new FakeFreePBX();
	$m = new \FreePBX\modules\Dmodem($fp);
	FreePBX::$modules['Dmodem'] = $m;
	$m->install();
	return $m;
}

/* the page's form posted: request r through doConfigPageInit() */
function dmodem_post($m, array $r)
{
	$_REQUEST = $_POST = $r + ['display' => 'dmodem'];
	$_SERVER['REQUEST_METHOD'] = 'POST';
	$m->errors = [];
	$m->posted = null;
	$m->message = null;
	$m->doConfigPageInit('dmodem');
}

/* the page for GET request r */
function dmodem_get($m, array $r)
{
	$_REQUEST = $_GET = $r + ['display' => 'dmodem'];
	$_POST = [];
	$_SERVER['REQUEST_METHOD'] = 'GET';
	return $m->showPage();
}

/* the dialplan FreePBX would write for the module */
function dmodem_dialplan($m)
{
	$ext = new extensions();
	$m->doDialplanHook($ext, 'asterisk', 500);
	return $ext->generateConf();
}
