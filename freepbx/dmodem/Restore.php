<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Neo-Desktop
//
// Restore.php - the module's settings and profiles out of a FreePBX
// backup (Dmodem::importData(): each checked as the forms check it).

namespace FreePBX\modules\Dmodem;

use FreePBX\modules\Backup as Base;

class Restore extends Base\RestoreBase
{
	/* the backup's data back into the module */
	public function runRestore()
	{
		$r = $this->FreePBX->Dmodem->importData($this->getConfigs());
		foreach ($r['settings'] as $field => $why)
			$this->log(sprintf(_('D-Modem: setting %s not restored: %s'), $field, $why), 'WARNING');
		foreach ($r['skipped'] as $name => $why)
			$this->log(sprintf(_('D-Modem: profile %s not restored: %s'), $name,
					   implode('; ', $why)), 'WARNING');
		$this->log(sprintf(_('D-Modem: %d profiles restored'), count($r['imported'])));
	}

	/* a FreePBX 13 backup: the module is newer, it has nothing there */
	public function processLegacy($pdo, $data, $tables, $unknownTables)
	{
	}
}
