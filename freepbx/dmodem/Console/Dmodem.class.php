<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Neo-Desktop
//
// Console/Dmodem.class.php - fwconsole dmodem (module.xml's console):
//
//   fwconsole dmodem dial <profile> <number> [--callerid=...]
//   fwconsole dmodem profiles
//   fwconsole dmodem export [file]           (stdout without a file)
//   fwconsole dmodem import <file> [--keep]  (--keep: profiles the file
//                                             hasn't stay)

namespace FreePBX\Console\Command;

use Symfony\Component\Console\Command\Command;
use Symfony\Component\Console\Input\InputArgument;
use Symfony\Component\Console\Input\InputInterface;
use Symfony\Component\Console\Input\InputOption;
use Symfony\Component\Console\Output\OutputInterface;

class Dmodem extends Command
{
	/* the command's name, arguments and options */
	protected function configure()
	{
		$this->setName('dmodem')
			->setDescription(_('D-Modem: dial out with a profile, list the profiles, export or import the settings'))
			->setDefinition([
				new InputArgument('args', InputArgument::IS_ARRAY,
					_('dial <profile> <number> | profiles | export [file] | import <file>')),
				new InputOption('callerid', null, InputOption::VALUE_REQUIRED,
					_('dial: the caller ID (else the settings\')'), ''),
				new InputOption('keep', null, InputOption::VALUE_NONE,
					_('import: keep the profiles the file hasn\'t')),
			]);
	}

	/* the subcommand run; 0 if it went through */
	protected function execute(InputInterface $input, OutputInterface $output)
	{
		return \FreePBX\modules\Dmodem\Cli::run(\FreePBX::Dmodem(),
			$input->getArgument('args'),
			['callerid' => (string)$input->getOption('callerid'),
			 'keep' => (bool)$input->getOption('keep')],
			function ($line) use ($output) {
				$output->writeln($line);
			});
	}
}
