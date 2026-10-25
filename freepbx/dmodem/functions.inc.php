<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Neo-Desktop
//
// functions.inc.php - FreePBX's destination hooks, which it looks up by
// the module's name: the profiles (and the fax receiver) as destinations
// of inbound routes, IVRs, time conditions and the like.

if (!defined('FREEPBX_IS_AUTH')) {
	die('No direct script access allowed');
}

/* the module's destinations */
function dmodem_destinations()
{
	return \FreePBX::Dmodem()->destinations();
}

/* the destination of extension exten of app-dmodem */
function dmodem_getdest($exten)
{
	return [\FreePBX\modules\Dmodem::destination($exten)];
}

/* what destination dest is (null: not ours) */
function dmodem_getdestinfo($dest)
{
	return \FreePBX::Dmodem()->destinationInfo($dest);
}
