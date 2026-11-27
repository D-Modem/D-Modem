<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// Console/Dmodem.class.php - fwconsole dmodem (module.xml's console):
//
//   fwconsole dmodem dial <profile> <number> [--callerid=...]
//   fwconsole dmodem profiles
//   fwconsole dmodem export [file]           (stdout without a file)
//   fwconsole dmodem import <file> [--keep]  (--keep: profiles the file
//                                             hasn't stay)
//   fwconsole dmodem fax2mail <recipient|""> <uniqueid> [status] [pages]
//                    [cid] [remote] [cidname] [rate]
//                                            (the fax of a call into the
//                                             fax log, and to its
//                                             recipient's email address)
//   fwconsole dmodem faxes                   (the fax log)
//   fwconsole dmodem sendfax <number> [file...] [--cover] [--to=...]
//                    [--from=...] [--message=...] [--retries=n]
//                    [--notify=never|always|success|failure] [--email=...]
//                                            (a fax into the queue)
//   fwconsole dmodem faxqueue                (the email to fax spool,
//                                             then the queue run:
//                                             FreePBX's jobs run it
//                                             each minute)
//   fwconsole dmodem mail2fax <file|->       (email to fax: a message
//                                             into the queue)
//   fwconsole dmodem faxjobs                 (the queue)
//   fwconsole dmodem faxcancel <job>         (a fax out of the queue)
//   fwconsole dmodem faxsent <job> <uniqueid> <status> [pages] [rate]
//                    [error] [remote]        (a try's result: the hangup
//                                             handler of a sent fax)

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
			->setDescription(_('D-Modem: dial out with a profile, list the profiles, export or import the settings, log and mail a received fax, list the fax log, send a fax'))
			->setDefinition([
				new InputArgument('args', InputArgument::IS_ARRAY,
					_('dial <profile> <number> | profiles | export [file] | import <file> | fax2mail <recipient> <uniqueid> ... | faxes | sendfax <number> [file...] | faxqueue | mail2fax <file|-> | faxjobs | faxcancel <job> | faxsent ...')),
				new InputOption('callerid', null, InputOption::VALUE_REQUIRED,
					_('dial: the caller ID (else the settings\')'), ''),
				new InputOption('keep', null, InputOption::VALUE_NONE,
					_('import: keep the profiles the file hasn\'t')),
				new InputOption('cover', null, InputOption::VALUE_NONE,
					_('sendfax: a cover page first')),
				new InputOption('to', null, InputOption::VALUE_REQUIRED,
					_('sendfax: the name on the cover page'), ''),
				new InputOption('from', null, InputOption::VALUE_REQUIRED,
					_('sendfax: the sender on the cover page'), ''),
				new InputOption('message', null, InputOption::VALUE_REQUIRED,
					_('sendfax: the message on the cover page'), ''),
				new InputOption('retries', null, InputOption::VALUE_REQUIRED,
					_('sendfax: the tries after the first (0 to 3; else the settings\')')),
				new InputOption('notify', null, InputOption::VALUE_REQUIRED,
					_('sendfax: when the result goes by email (else the settings\')')),
				new InputOption('email', null, InputOption::VALUE_REQUIRED,
					_('sendfax: the address for the result (else the settings\')')),
			]);
	}

	/* the subcommand run; 0 if it went through */
	protected function execute(InputInterface $input, OutputInterface $output)
	{
		return \FreePBX\modules\Dmodem\Cli::run(\FreePBX::Dmodem(),
			$input->getArgument('args'),
			array_filter([
				'callerid' => (string)$input->getOption('callerid'),
				'keep' => (bool)$input->getOption('keep'),
				'cover' => (bool)$input->getOption('cover'),
				'to' => $input->getOption('to'),
				'from' => $input->getOption('from'),
				'message' => $input->getOption('message'),
				'retries' => $input->getOption('retries'),
				'notify' => $input->getOption('notify'),
				'email' => $input->getOption('email'),
			], function ($v) {
				return $v !== null;
			}),
			function ($line) use ($output) {
				$output->writeln($line);
			});
	}
}
