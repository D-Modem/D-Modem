<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// backup_env.php - what the module's Backup.php and Restore.php use of
// FreePBX's backup module (its BackupBase and RestoreBase, as its
// Models/Backup.php and Models/Restore.php have them): the constructors'
// arguments, addConfigs(), getConfigs(), log().

namespace FreePBX\modules\Backup;

/* Models\ModelBase: the module's data and the log */
class ModelBase
{
	public $FreePBX;
	public $data = ['configs' => [], 'files' => [], 'dirs' => [],
			'dependencies' => [], 'garbage' => []];
	public $logged = [];

	/* as the backup module constructs a module's class */
	public function __construct($freepbx, $backupModVer, $logger, $transactionId,
				    $modData, $defaultFallback)
	{
		$this->FreePBX = $freepbx;
		foreach ($this->data as $k => $v)
			if (!isset($modData[$k]))
				$modData[$k] = $v;
		$this->data = $modData;
	}

	/* the configs the backup has */
	public function getConfigs($options = [])
	{
		return $this->data['configs'];
	}

	/* a line of the backup's log */
	protected function log($message = '', $level = 'INFO')
	{
		$this->logged[] = [$level, $message];
	}
}

/* BackupBase: what runBackup() hands the backup */
class BackupBase extends ModelBase
{
	/* settings as the module's configs */
	public function addConfigs($settings)
	{
		if (empty($settings))
			return;
		$this->data['configs'] = $settings;
	}
}

/* RestoreBase: a restore's constructor has the backup's temporary dir */
class RestoreBase extends ModelBase
{
	/* as the backup module constructs a module's Restore */
	public function __construct($freepbx, $backupModVer, $logger, $transactionId,
				    $modData, $backupTmpDir, $defaultFallback)
	{
		parent::__construct($freepbx, $backupModVer, $logger, $transactionId,
				    $modData, $defaultFallback);
	}
}
