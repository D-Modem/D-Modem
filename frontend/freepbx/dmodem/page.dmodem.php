<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// page.dmodem.php - Applications > D-Modem (Dmodem::showPage())

if (!defined('FREEPBX_IS_AUTH')) {
	die('No direct script access allowed');
}
echo \FreePBX::Dmodem()->showPage();
