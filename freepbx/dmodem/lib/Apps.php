<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// Apps.php - a dialplan step of any application for FreePBX's
// extensions class (DModem(), Stasis(), DModemReceiveFax()), which has a
// class of its own for its core applications only.  Included by
// Dmodem::doDialplanHook(), when FreePBX's extension class is loaded.

if (class_exists('extension', false) && !class_exists('ext_dmodem_app', false)) {
	class ext_dmodem_app extends extension
	{
		public $app;

		/* application app with its arguments data */
		public function __construct($app, $data = '')
		{
			$this->app = $app;
			$this->data = $data;
		}

		/* the step as extensions.conf has it: App(data) */
		public function output()
		{
			return $this->app . '(' . $this->data . ')';
		}
	}
}
