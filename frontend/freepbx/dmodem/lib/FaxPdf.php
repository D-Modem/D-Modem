<?php
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// FaxPdf.php - a received fax (a TIFF file) as a PDF file, for the
// fax-to-email attachment.  The TIFF reader takes what fax TIFFs have:
// 1 bit pels in T.4 (MH or MR) or T.6 (MMR) coding or uncompressed, and
// 8 bit grey or RGB pels uncompressed (a colour fax).  The coded strips
// go into the PDF as they are (the CCITTFaxDecode filter), so nothing is
// decoded here.  Each page fits the paper size (Letter or A4), at the
// top, in the middle.

namespace FreePBX\modules\Dmodem;

class FaxPdf
{
	/* the paper sizes in points (1/72 inch): width, height */
	const PAPER = [
		'letter' => [612.0, 792.0],
		'a4' => [595.28, 841.89],
	];
	/* the most pages and strips that a file may have */
	const MAX_PAGES = 500;
	const MAX_STRIPS = 10000;

	/* the TIFF file's bytes b: its byte order, then the reader's
	   functions over it */
	private $b;
	private $le;

	/* a reader of the TIFF bytes b */
	private function __construct($b)
	{
		$this->b = $b;
		$this->le = substr($b, 0, 2) === 'II';
	}

	/* the unsigned 16 bit number at offset at */
	private function u16($at)
	{
		if ($at < 0 || $at + 2 > strlen($this->b))
			throw new \RuntimeException('past the end of the file');
		$v = unpack($this->le ? 'v' : 'n', $this->b, $at);
		return $v[1];
	}

	/* the unsigned 32 bit number at offset at */
	private function u32($at)
	{
		if ($at < 0 || $at + 4 > strlen($this->b))
			throw new \RuntimeException('past the end of the file');
		$v = unpack($this->le ? 'V' : 'N', $this->b, $at);
		return $v[1];
	}

	/* the values of the IFD entry at e (its type: 3 SHORT, 4 LONG, 5
	   RATIONAL as a float), a list */
	private function values($e)
	{
		$type = $this->u16($e + 2);
		$count = $this->u32($e + 4);
		$size = [3 => 2, 4 => 4, 5 => 8];
		if (!isset($size[$type]) || $count > self::MAX_STRIPS)
			return [];
		$at = $count * $size[$type] <= 4 ? $e + 8 : $this->u32($e + 8);
		$out = [];
		for ($i = 0; $i < $count; $i++) {
			if ($type === 3) {
				$out[] = $this->u16($at + 2 * $i);
			} elseif ($type === 4) {
				$out[] = $this->u32($at + 4 * $i);
			} else {
				$d = $this->u32($at + 8 * $i + 4);
				$out[] = $d ? $this->u32($at + 8 * $i) / $d : 0.0;
			}
		}
		return $out;
	}

	/* the bytes of a strip, the bits of each byte the other way round
	   when the fill order is 2 (the last bit first) */
	private function strip($at, $len, $fill)
	{
		if ($at < 0 || $len < 0 || $at + $len > strlen($this->b))
			throw new \RuntimeException('a strip past the end of the file');
		$s = substr($this->b, $at, $len);
		if ($fill === 2) {
			static $rev = null;
			if ($rev === null) {
				$from = $to = '';
				for ($i = 0; $i < 256; $i++) {
					$r = 0;
					for ($k = 0; $k < 8; $k++)
						if ($i & (1 << $k))
							$r |= 0x80 >> $k;
					$from .= chr($i);
					$to .= chr($r);
				}
				$rev = [$from, $to];
			}
			$s = strtr($s, $rev[0], $rev[1]);
		}
		return $s;
	}

	/* the pages of the TIFF bytes b: each [width, rows, xres, yres
	   (pels an inch), strips], a strip [rows, PDF image dictionary
	   entries, data].  An exception if the file is not a TIFF that
	   this reader takes */
	public static function pages($b)
	{
		$t = new self($b);
		$magic = substr($b, 0, 2);
		if (($magic !== 'II' && $magic !== 'MM') || $t->u16(2) !== 42)
			throw new \RuntimeException('not a TIFF file');
		$pages = [];
		$ifd = $t->u32(4);
		$seen = [];
		while ($ifd && count($pages) < self::MAX_PAGES) {
			if (isset($seen[$ifd]))
				break;
			$seen[$ifd] = true;
			$pages[] = $t->page($ifd);
			$ifd = $t->u32($ifd + 2 + 12 * $t->u16($ifd));
		}
		if (!$pages)
			throw new \RuntimeException('no page');
		return $pages;
	}

	/* the page of the IFD at ifd (see pages()) */
	private function page($ifd)
	{
		$tag = [];
		$n = $this->u16($ifd);
		for ($i = 0; $i < $n; $i++) {
			$e = $ifd + 2 + 12 * $i;
			$tag[$this->u16($e)] = $this->values($e);
		}
		$get = function ($k, $def) use ($tag) {
			return isset($tag[$k][0]) ? $tag[$k][0] : $def;
		};
		$w = (int)$get(256, 0);
		$h = (int)$get(257, 0);
		$bps = (int)$get(258, 1);
		$spp = (int)$get(277, 1);
		$comp = (int)$get(259, 1);
		$photo = (int)$get(262, 0);
		$fill = (int)$get(266, 1);
		$rps = (int)$get(278, $h);
		$t4 = (int)$get(292, 0);
		$unit = (int)$get(296, 2);
		$xres = (float)$get(282, 0);
		$yres = (float)$get(283, 0);
		$offs = isset($tag[273]) ? $tag[273] : [];
		$lens = isset($tag[279]) ? $tag[279] : [];
		if ($w < 1 || $h < 1 || $w > 20000 || $h > 100000)
			throw new \RuntimeException('a page of no size');
		if (!$offs || count($offs) !== count($lens))
			throw new \RuntimeException('a page without its strips');
		if ($unit === 3) {
			/* pels a centimetre */
			$xres *= 2.54;
			$yres *= 2.54;
		}
		if ($xres <= 0)
			$xres = $w >= 1700 && $w <= 1760 ? 204.0 : 200.0;
		if ($yres <= 0)
			$yres = $xres;
		if ($rps < 1 || $rps > $h)
			$rps = $h;
		if ($bps === 1 && $spp === 1) {
			$base = ['/ColorSpace /DeviceGray', '/BitsPerComponent 1'];
			if ($comp === 1) {
				$filter = null;
				/* uncompressed: 0 is white for photometric 0 */
				if ($photo === 0)
					$base[] = '/Decode [1 0]';
			} elseif ($comp === 3 || $comp === 4) {
				$k = $comp === 4 ? -1 : (($t4 & 1) ? 1 : 0);
				$parms = '<< /K ' . $k . ' /Columns ' . $w;
				if ($comp === 3)
					$parms .= ' /EndOfLine true' .
						(($t4 & 4) ? ' /EncodedByteAlign true' : '');
				if ($photo === 1)
					$base[] = '/Decode [1 0]';
				$filter = ['/CCITTFaxDecode', $parms];
			} else {
				throw new \RuntimeException('a coding this reader does not take (' . $comp . ')');
			}
		} elseif ($bps === 8 && ($spp === 1 || $spp === 3) && $comp === 1) {
			$base = ['/ColorSpace ' . ($spp === 3 ? '/DeviceRGB' : '/DeviceGray'),
				 '/BitsPerComponent 8'];
			if ($spp === 1 && $photo === 0)
				$base[] = '/Decode [1 0]';
			$filter = function_exists('gzcompress') ? ['/FlateDecode', null] : null;
		} else {
			throw new \RuntimeException('pels this reader does not take');
		}
		$strips = [];
		$left = $h;
		foreach ($offs as $i => $at) {
			if ($left <= 0)
				break;
			$rows = min($rps, $left);
			$left -= $rows;
			$data = $this->strip((int)$at, (int)$lens[$i], $fill);
			$d = $base;
			if (is_array($filter)) {
				$d[] = '/Filter ' . $filter[0];
				if ($filter[0] === '/FlateDecode')
					$data = gzcompress($data);
				elseif ($filter[1] !== null)
					$d[] = '/DecodeParms ' . $filter[1] . ' /Rows ' . $rows . ' >>';
			}
			$strips[] = [$rows, $d, $data];
		}
		return [$w, $h, $xres, $yres, $strips];
	}

	/* a number for the PDF: no more than 4 decimals, no exponent */
	private static function num($v)
	{
		$s = rtrim(rtrim(sprintf('%.4F', $v), '0'), '.');
		return $s === '-0' || $s === '' ? '0' : $s;
	}

	/* the PDF of the TIFF bytes b on paper (a PAPER key; else Letter).
	   An exception if the TIFF is not one that pages() takes */
	public static function fromTiff($b, $paper = 'letter')
	{
		$pages = self::pages($b);
		list($pw, $ph) = isset(self::PAPER[$paper]) ? self::PAPER[$paper] :
			self::PAPER['letter'];
		/* objects: 1 the catalog, 2 the page tree, then each page's
		   page, contents and strips */
		$obj = [1 => '', 2 => ''];
		$kids = [];
		foreach ($pages as $p) {
			list($w, $h, $xres, $yres, $strips) = $p;
			$iw = $w / $xres * 72.0;
			$ih = $h / $yres * 72.0;
			$s = min(1.0, $pw / $iw, $ph / $ih);
			$x = ($pw - $iw * $s) / 2;
			$top = $ph;
			$page = count($obj) + 1;
			$cont = $page + 1;
			$kids[] = $page . ' 0 R';
			$obj[$page] = '';
			$obj[$cont] = '';
			$ops = '';
			$res = '';
			$y = $top;
			foreach ($strips as $i => $st) {
				list($rows, $dict, $data) = $st;
				$id = count($obj) + 1;
				$obj[$id] = "<< /Type /XObject /Subtype /Image /Width " . $w .
					" /Height " . $rows . ' ' . implode(' ', $dict) .
					" /Length " . strlen($data) . " >>\nstream\n" . $data .
					"\nendstream";
				$sh = $rows / $yres * 72.0 * $s;
				$y -= $sh;
				$ops .= 'q ' . self::num($iw * $s) . ' 0 0 ' . self::num($sh) .
					' ' . self::num($x) . ' ' . self::num($y) .
					' cm /S' . $i . " Do Q\n";
				$res .= '/S' . $i . ' ' . $id . ' 0 R ';
			}
			$obj[$page] = '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ' .
				self::num($pw) . ' ' . self::num($ph) . '] /Resources << /XObject << ' .
				$res . '>> >> /Contents ' . $cont . ' 0 R >>';
			$obj[$cont] = '<< /Length ' . strlen($ops) . " >>\nstream\n" . $ops .
				"endstream";
		}
		$obj[1] = '<< /Type /Catalog /Pages 2 0 R >>';
		$obj[2] = '<< /Type /Pages /Kids [' . implode(' ', $kids) . '] /Count ' .
			count($kids) . ' >>';
		$out = "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n";
		$at = [];
		foreach ($obj as $id => $o) {
			$at[$id] = strlen($out);
			$out .= $id . " 0 obj\n" . $o . "\nendobj\n";
		}
		$xref = strlen($out);
		$out .= "xref\n0 " . (count($obj) + 1) . "\n0000000000 65535 f \n";
		foreach ($at as $off)
			$out .= sprintf("%010d 00000 n \n", $off);
		$out .= "trailer\n<< /Size " . (count($obj) + 1) .
			" /Root 1 0 R >>\nstartxref\n" . $xref . "\n%%EOF\n";
		return $out;
	}
}
