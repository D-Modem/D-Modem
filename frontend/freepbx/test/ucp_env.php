<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// ucp_env.php - stand-ins for the parts of FreePBX's UCP that the module
// uses, for unit.php: the Ucp BMO's settings of users and groups
// (FakeUcp), and UCP's module base class (\UCP\Modules) with the signed-in
// user, for ucp/Dmodem.class.php.

namespace {

/* the Ucp BMO: settings by user id and by group id; a user's group */
class FakeUcp
{
	public $byId = [];
	public $byGid = [];
	public $groupOf = [];

	/* setting of module for user id, null if not set */
	public function getSettingByID($id, $module, $setting)
	{
		return isset($this->byId[$id][$module][$setting]) ? $this->byId[$id][$module][$setting] : null;
	}

	/* setting of module for group gid, null if not set */
	public function getSettingByGID($gid, $module, $setting)
	{
		return isset($this->byGid[$gid][$module][$setting]) ? $this->byGid[$gid][$module][$setting] : null;
	}

	/* the user's setting, else its group's */
	public function getCombinedSettingByID($id, $module, $setting)
	{
		$v = $this->getSettingByID($id, $module, $setting);
		if ($v === null && isset($this->groupOf[$id]))
			$v = $this->getSettingByGID($this->groupOf[$id], $module, $setting);
		return $v;
	}

	/* setting of module for user id kept (null: not set) */
	public function setSettingByID($id, $module, $setting, $value)
	{
		$this->byId[$id][$module][$setting] = $value;
	}

	/* setting of module for group gid kept */
	public function setSettingByGID($gid, $module, $setting, $value)
	{
		$this->byGid[$gid][$module][$setting] = $value;
	}
}

}

namespace UCP {

/* UCP's module base class: $this->UCP (FreePBX, User) and load_view() */
class Modules
{
	public static $ucp;

	/* $this->UCP: the UCP object of the test */
	public function __get($name)
	{
		return $name === 'UCP' ? self::$ucp : null;
	}

	/* view file f with vars, as HTML */
	protected function load_view($f, $vars = [])
	{
		extract($vars);
		ob_start();
		include $f;
		return ob_get_clean();
	}
}

/* UCP's user object: the signed-in user */
class FakeUser
{
	public $user = false;

	/* the signed-in user, false if none */
	public function getUser()
	{
		return $this->user;
	}
}

}
