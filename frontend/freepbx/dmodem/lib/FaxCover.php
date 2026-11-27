<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// lib/FaxCover.php - the cover page of a fax that D-Modem sends, as a PDF
// file of one page (our own code: text in the standard Helvetica fonts,
// no font files).  FaxSend gives it to Ghostscript with the documents,
// which makes the fax pages of all of them.

namespace FreePBX\modules\Dmodem;

class FaxCover
{
	/* the paper sizes in points: width, height */
	const PAPER = ['letter' => [612, 792], 'a4' => [595, 842]];
	/* the most lines of the message on the page */
	const MAX_LINES = 36;
	/* the characters of a line of the message (11 pt Helvetica on the
	   width of the page, with margins) */
	const WRAP = 88;

	/* s as a PDF string: in WinAnsi (the standard fonts' encoding),
	   with ( ) and \ escaped, other characters as ? */
	public static function str($s)
	{
		$s = (string)$s;
		$w = function_exists('iconv') ?
			@iconv('UTF-8', 'Windows-1252//TRANSLIT', $s) : false;
		if ($w === false)
			$w = preg_replace('/[^\x20-\x7e]/', '?', $s);
		$w = preg_replace('/[\x00-\x1f\x7f]/', ' ', $w);
		return '(' . strtr($w, ['\\' => '\\\\', '(' => '\\(', ')' => '\\)']) . ')';
	}

	/* the lines of text t wrapped at width characters, at most max
	   lines (the last one ends with ... when the text is longer) */
	public static function wrap($t, $width = self::WRAP, $max = self::MAX_LINES)
	{
		$out = [];
		foreach (preg_split('/\r\n|\r|\n/', (string)$t) as $para) {
			$w = $para === '' ? [''] :
				explode("\n", wordwrap($para, $width, "\n", true));
			foreach ($w as $l)
				$out[] = $l;
		}
		if (count($out) > $max) {
			$out = array_slice($out, 0, $max);
			$out[$max - 1] = rtrim(substr($out[$max - 1], 0, $width - 3)) . '...';
		}
		return $out;
	}

	/* the PDF of the cover page on paper (a PAPER key; else Letter).
	   c: 'name', 'address' (lines), 'phone', 'web', 'footer' (the
	   company: the settings), and 'to', 'number', 'from', 'pages' (0:
	   not shown), 'date', 'message' (this fax) */
	public static function pdf(array $c, $paper = 'letter')
	{
		list($pw, $ph) = isset(self::PAPER[$paper]) ? self::PAPER[$paper] :
			self::PAPER['letter'];
		$v = function ($k) use ($c) {
			return isset($c[$k]) ? trim((string)$c[$k]) : '';
		};
		$ops = [];
		/* text t at x, y in font f (F1 Helvetica, F2 bold) of size */
		$text = function ($x, $y, $f, $size, $t) use (&$ops) {
			$ops[] = sprintf('BT /%s %d Tf %d %d Td %s Tj ET', $f, $size,
					 $x, $y, self::str($t));
		};
		$line = function ($y) use (&$ops, $pw) {
			$ops[] = sprintf('0.5 w 50 %d m %d %d l S', $y, $pw - 50, $y);
		};
		$y = $ph - 80;
		$text(50, $y, 'F2', 40, 'FAX');
		/* the company, on the right */
		$cy = $ph - 60;
		$right = [];
		if ($v('name') !== '')
			$right[] = ['F2', 14, $v('name')];
		foreach (preg_split('/\r\n|\r|\n/', $v('address')) as $l)
			if (trim($l) !== '')
				$right[] = ['F1', 10, trim($l)];
		foreach (['phone', 'web'] as $k)
			if ($v($k) !== '')
				$right[] = ['F1', 10, $v($k)];
		foreach ($right as $r) {
			$text((int)($pw / 2), $cy, $r[0], $r[1], $r[2]);
			$cy -= $r[1] + 4;
		}
		$y = min($y, $cy) - 20;
		$line($y);
		$y -= 30;
		$rows = [
			[_('Date:'), $v('date')],
			[_('To:'), $v('to')],
			[_('Fax number:'), $v('number')],
			[_('From:'), $v('from')],
		];
		if (!empty($c['pages']))
			$rows[] = [_('Pages:'), sprintf(_('%d, with this page'), (int)$c['pages'])];
		foreach ($rows as $r) {
			$text(50, $y, 'F2', 12, $r[0]);
			$text(150, $y, 'F1', 12, $r[1]);
			$y -= 20;
		}
		$y -= 5;
		$line($y);
		$y -= 25;
		foreach (self::wrap($v('message')) as $l) {
			if ($l !== '')
				$text(50, $y, 'F1', 11, $l);
			$y -= 15;
		}
		if ($v('footer') !== '') {
			$line(60);
			$text(50, 45, 'F1', 9, $v('footer'));
		}
		$content = implode("\n", $ops) . "\n";
		$obj = [
			1 => '<< /Type /Catalog /Pages 2 0 R >>',
			2 => '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
			3 => sprintf('<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] ' .
				     '/Resources << /Font << /F1 5 0 R /F2 6 0 R >> >> ' .
				     '/Contents 4 0 R >>', $pw, $ph),
			4 => '<< /Length ' . strlen($content) . " >>\nstream\n" .
				$content . 'endstream',
			5 => '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica ' .
				'/Encoding /WinAnsiEncoding >>',
			6 => '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold ' .
				'/Encoding /WinAnsiEncoding >>',
		];
		$pdf = "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n";
		$off = [];
		foreach ($obj as $n => $o) {
			$off[$n] = strlen($pdf);
			$pdf .= "$n 0 obj\n$o\nendobj\n";
		}
		$x = strlen($pdf);
		$pdf .= "xref\n0 " . (count($obj) + 1) . "\n0000000000 65535 f \n";
		foreach ($off as $o)
			$pdf .= sprintf("%010d 00000 n \n", $o);
		$pdf .= "trailer\n<< /Size " . (count($obj) + 1) . " /Root 1 0 R >>\n" .
			"startxref\n$x\n%%EOF\n";
		return $pdf;
	}
}
