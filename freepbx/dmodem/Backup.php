<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Neo-Desktop
//
// Backup.php - the module in FreePBX's Backup & Restore (the backup
// module looks for FreePBX\modules\<Module>\Backup): its settings and
// profiles (Dmodem::exportData()).  dmodem.conf and the dialplan are
// made from them again, so they are not kept.

namespace FreePBX\modules\Dmodem;

use FreePBX\modules\Backup as Base;

class Backup extends Base\BackupBase
{
	/* the module's data into the backup */
	public function runBackup($id, $transaction)
	{
		$data = $this->FreePBX->Dmodem->exportData();
		$this->log(sprintf(_('D-Modem: %d profiles'), count($data['profiles'])));
		$this->addConfigs($data);
	}
}
