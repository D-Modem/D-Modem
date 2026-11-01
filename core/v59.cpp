/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v59.cpp - V.59's managed objects for diagnostic information, read
 *              with V.250's +TMO (6.9/V.250)
 *
 *    V.59 (11/2000) names the facts about the last call a modem can
 *    give (its identity and capabilities, the modulation's measures,
 *    the call's progress and why it ended, the error correction, the
 *    compression and the DTE interface), each object with a name and a
 *    four-digit hexadecimal Tag-ID: the two high digits its high-level
 *    object (V90Diag 09, CnxDiag 2C, ...), the two low ones the object
 *    within it, 40 to 5F the "common" objects every modulation's
 *    high-level object may have (txDataHistory 4C: 094C for V.90, 0C4C
 *    for V.34).  The objects are kept from the start of a call until
 *    the start of the next (ATZ clears them, as AT#UD's log), and an
 *    object that does not apply to the last call is NULL.
 *
 *    +TMO's forms (blanks are dropped from command lines, case does not
 *    matter):
 *
 *      +TMO                    the last +TMO again
 *      +TMO <level>[N]=?       the objects there are, as tags (N: as
 *                              names): 0 all, 1 the high-level ones, 2
 *                              the mid-level ones, 3 the low-level ones
 *                              (txPower); 4: "(0,1)", names and tags
 *      +TMO <tag>              a four-digit tag: that object; two
 *                              digits: every object of that high-level
 *                              object ("All" after either: the same)
 *      +TMO <name> [All]       an object by name, or every object of a
 *                              high-level one (V90Diag, or V90 alone)
 *      +TMO <high> <name>      the object of that high-level object
 *
 *    each object a line <tag>{<value>} or <name>{<value>}, as the
 *    request named it.  A common object asked by name alone is the last
 *    call's modulation's (6.9.1/V.250's note).  Values as 6.2/V.59 has
 *    them: numbers in decimal, units with levels ("-14.8dBm0", "37ms"),
 *    TRUE and FALSE, the codes of V.59's tables, octet sequences in
 *    hexadecimal separated by ";", and a BIT STRING as the hexadecimal
 *    number of its bits (bit 0 the least significant).
 *
 *    Only the objects whose facts the core keeps are here; the ones
 *    made of the exchanged sequences it does not record (V.8's CI, V.34's
 *    INFO and MP, V.90's CP, V.92's QC, V.8 bis's) are not.
 */
#include <algorithm>
#include <cmath>
#include <cstring>

#include <dsplibs.h>
#include <link.h>
#include <modem_dp.h>

#include "dm_types.h"
#include "dcore.h"
#include "str.h"

namespace dcore {

namespace {

/* the high-level objects (6.3/V.59) this modem has, their tags (the
   high digits) and names, with a short name for a modulation's (+TMO
   V92 All, as 6.9.3/V.250's example has it) */
struct V59High {
	u8 tag;
	const char *name, *alias;
};
const V59High v59_high[] = {
	{ 0x00, "ModemIdentity", nullptr },
	{ 0x01, "ModemCapability", nullptr },
	{ 0x02, "ModeSelected", nullptr },
	{ 0x07, "V8Diag", "V8" },
	{ 0x09, "V90Diag", "V90" },
	{ 0x0A, "V91Diag", "V91" },
	{ 0x0B, "V92ModDiag", "V92" },
	{ 0x0C, "V34Diag", "V34" },
	{ 0x0D, "V32Diag", "V32" },
	{ 0x0E, "V22Diag", "V22" },
	{ 0x0F, "V23Diag", "V23" },
	{ 0x10, "V21Diag", "V21" },
	{ 0x15, "V34HdxDiag", "V34H" },
	{ 0x16, "V17Diag", "V17" },
	{ 0x17, "V29faxDiag", "V29" },
	{ 0x18, "V27tfaxDiag", "V27T" },
	{ 0x23, "V18Diag", "V18" },
	{ 0x26, "NSMDiag", "NSM" },
	{ 0x2C, "CnxDiag", nullptr },
	{ 0x2E, "CallProgress", nullptr },
	{ 0x2F, "ErrorControl", nullptr },
	{ 0x30, "Compression", nullptr },
	{ 0x31, "DTEDCE", nullptr },
};

/* the objects, in the order of V.59's clauses: the high-level object
   each belongs to (ModemCapability's are the modulations' and the
   protocols' mode objects, listed with it too), its tag and name */
struct V59Obj {
	u8 high;
	u16 tag;
	const char *name;
};
const V59Obj v59_objs[] = {
	/* 6.5 */
	{ 0x00, 0x0001, "manufacturer" },
	{ 0x00, 0x0002, "model" },
	{ 0x00, 0x0003, "firmwareVersion" },
	{ 0x00, 0x0004, "diagnosticVersion" },
	{ 0x00, 0x0005, "multimediaMode" },
	/* 6.6 */
	{ 0x01, 0x0900, "modeV90" },
	{ 0x01, 0x0A00, "modeV91" },
	{ 0x01, 0x0B00, "modeV92" },
	{ 0x01, 0x0C00, "modeV34" },
	{ 0x01, 0x0D00, "modeV32B" },
	{ 0x01, 0x0E00, "modeV22B" },
	{ 0x01, 0x0F00, "modeV23" },
	{ 0x01, 0x1000, "modeV21" },
	{ 0x01, 0x1500, "modeV34H" },
	{ 0x01, 0x1600, "modeV17" },
	{ 0x01, 0x1700, "modeV29" },
	{ 0x01, 0x1800, "modeV27T" },
	{ 0x01, 0x2300, "modeV18" },
	{ 0x01, 0x0101, "otherModMode" },
	{ 0x01, 0x2F00, "modeV42" },
	{ 0x01, 0x3000, "modeV42B" },
	{ 0x01, 0x300A, "modeV44" },
	{ 0x01, 0x0102, "otherProtMode" },
	/* 6.7 */
	{ 0x02, 0x0201, "modulationHistory" },
	/* 6.8.1 */
	{ 0x07, 0x0702, "cm" },
	{ 0x07, 0x0703, "jm" },
	{ 0x07, 0x0704, "v8Result" },
	/* 6.8.3 */
	{ 0x09, 0x0900, "modeV90" },
	{ 0x09, 0x0947, "txPower" },
	{ 0x09, 0x0948, "txSymbolRate" },
	{ 0x09, 0x0949, "rxSymbolRate" },
	{ 0x09, 0x094A, "txCarrierFreq" },
	{ 0x09, 0x094B, "rxCarrierFreq" },
	{ 0x09, 0x094C, "txDataHistory" },
	{ 0x09, 0x094D, "rxDataHistory" },
	{ 0x09, 0x094E, "rxLevelEstimate" },
	{ 0x09, 0x094F, "noiseEstimate" },
	{ 0x09, 0x0951, "nearEchoEstimate" },
	{ 0x09, 0x0952, "farEchoEstimate" },
	{ 0x09, 0x0953, "roundTripDelay" },
	{ 0x09, 0x0955, "rBSPattern" },
	{ 0x09, 0x0956, "digitalPadLoss" },
	{ 0x09, 0x0907, "codecLaw" },
	/* 6.8.4 (its table numbers the common objects one off from 6.4's
	   Tag-IDs; these are 6.4's) */
	{ 0x0A, 0x0A00, "modeV91" },
	{ 0x0A, 0x0A47, "txPower" },
	{ 0x0A, 0x0A4C, "txDataHistory" },
	{ 0x0A, 0x0A4D, "rxDataHistory" },
	{ 0x0A, 0x0A4F, "noiseEstimate" },
	{ 0x0A, 0x0A55, "rBSPattern" },
	{ 0x0A, 0x0A04, "localCodecLaw" },
	{ 0x0A, 0x0A05, "remoteCodecLaw" },
	/* 6.8.5 */
	{ 0x0B, 0x0B00, "modeV92" },
	{ 0x0B, 0x0B47, "txPower" },
	{ 0x0B, 0x0B48, "v92SymbolRate" },
	{ 0x0B, 0x0B4C, "txDataHistory" },
	{ 0x0B, 0x0B4D, "rxDataHistory" },
	{ 0x0B, 0x0B4E, "rxLevelEstimate" },
	{ 0x0B, 0x0B4F, "noiseEstimate" },
	{ 0x0B, 0x0B51, "nearEchoEstimate" },
	{ 0x0B, 0x0B52, "farEchoEstimate" },
	{ 0x0B, 0x0B53, "roundTripDelay" },
	{ 0x0B, 0x0B55, "rBSPattern" },
	{ 0x0B, 0x0B56, "digitalPadLoss" },
	{ 0x0B, 0x0B07, "localCodecLaw" },
	{ 0x0B, 0x0B08, "remoteCodecLaw" },
	{ 0x0B, 0x0B21, "mOHEnable" },
	/* 6.8.6 */
	{ 0x0C, 0x0C00, "modeV34" },
	{ 0x0C, 0x0C47, "txPower" },
	{ 0x0C, 0x0C48, "txSymbolRate" },
	{ 0x0C, 0x0C49, "rxSymbolRate" },
	{ 0x0C, 0x0C4A, "txCarrierFreq" },
	{ 0x0C, 0x0C4B, "rxCarrierFreq" },
	{ 0x0C, 0x0C4C, "txDataHistory" },
	{ 0x0C, 0x0C4D, "rxDataHistory" },
	{ 0x0C, 0x0C4E, "rxLevelEstimate" },
	{ 0x0C, 0x0C4F, "noiseEstimate" },
	{ 0x0C, 0x0C51, "nearEchoEstimate" },
	{ 0x0C, 0x0C52, "farEchoEstimate" },
	{ 0x0C, 0x0C53, "roundTripDelay" },
	/* 6.8.7 */
	{ 0x0D, 0x0D00, "modeV32B" },
	{ 0x0D, 0x0D47, "txPower" },
	{ 0x0D, 0x0D4C, "txDataHistory" },
	{ 0x0D, 0x0D4D, "rxDataHistory" },
	{ 0x0D, 0x0D4E, "rxLevelEstimate" },
	{ 0x0D, 0x0D4F, "noiseEstimate" },
	{ 0x0D, 0x0D51, "nearEchoEstimate" },
	{ 0x0D, 0x0D52, "farEchoEstimate" },
	{ 0x0D, 0x0D53, "roundTripDelay" },
	/* 6.8.8 */
	{ 0x0E, 0x0E00, "modeV22B" },
	{ 0x0E, 0x0E47, "txPower" },
	{ 0x0E, 0x0E4C, "dataRateHistory" },
	{ 0x0E, 0x0E4E, "rxLevelEstimate" },
	{ 0x0E, 0x0E4F, "noiseEstimate" },
	/* 6.8.9 */
	{ 0x0F, 0x0F00, "modeV23" },
	{ 0x0F, 0x0F47, "txPower" },
	{ 0x0F, 0x0F01, "duplex" },
	{ 0x0F, 0x0F02, "transmitDataRate" },
	{ 0x0F, 0x0F03, "receiveDataRate" },
	{ 0x0F, 0x0F4E, "rxLevelEstimate" },
	/* 6.8.10 */
	{ 0x10, 0x1000, "modeV21" },
	{ 0x10, 0x1047, "txPower" },
	{ 0x10, 0x104E, "rxLevelEstimate" },
	/* 6.8.11 to 6.8.14: the fax modulations' modes */
	{ 0x15, 0x1500, "modeV34H" },
	{ 0x16, 0x1600, "modeV17" },
	{ 0x17, 0x1700, "modeV29" },
	{ 0x18, 0x1800, "modeV27T" },
	/* 6.8.15 */
	{ 0x23, 0x2300, "modeV18" },
	{ 0x23, 0x2347, "txPower" },
	{ 0x23, 0x234E, "rxLevelEstimate" },
	/* 6.8.18: the modulations V.59 has no object for (K56flex, V.32terbo,
	   V.26 ter, Bell's, V.cvm, ...) */
	{ 0x26, 0x2601, "proprietaryMode" },
	/* 6.9 */
	{ 0x2C, 0x2C01, "txCarrier" },
	{ 0x2C, 0x2C02, "rxCarrier" },
	{ 0x2C, 0x2C03, "carrierLosses" },
	{ 0x2C, 0x2C04, "renegRequests" },
	{ 0x2C, 0x2C05, "renegSuccesses" },
	{ 0x2C, 0x2C06, "retrainRequests" },
	{ 0x2C, 0x2C07, "retrainSuccesses" },
	{ 0x2C, 0x2C09, "duration" },
	{ 0x2C, 0x2C0A, "caller" },
	/* 6.11 */
	{ 0x2E, 0x2E01, "callProgressResult" },
	{ 0x2E, 0x2E02, "callTerminate" },
	/* 6.12 */
	{ 0x2F, 0x2F00, "modeV42" },
	{ 0x2F, 0x2F01, "protocolNegotiation" },
	{ 0x2F, 0x2F03, "txFrameSize" },
	{ 0x2F, 0x2F04, "rxFrameSize" },
	{ 0x2F, 0x2F05, "txWindow" },
	{ 0x2F, 0x2F06, "rxWindow" },
	{ 0x2F, 0x2F07, "linkTimeout" },
	{ 0x2F, 0x2F08, "framesSentAck" },
	{ 0x2F, 0x2F09, "framesRetransmitted" },
	{ 0x2F, 0x2F0A, "framesReceivedAck" },
	{ 0x2F, 0x2F0B, "framesReceivedDiscard" },
	{ 0x2F, 0x2F0C, "txErrors" },
	{ 0x2F, 0x2F0D, "rxErrors" },
	{ 0x2F, 0x2F0E, "txThroughput" },
	{ 0x2F, 0x2F0F, "rxThroughput" },
	{ 0x30, 0x3000, "modeV42B" },
	{ 0x30, 0x3001, "compressionNegotiationResult" },
	{ 0x30, 0x3002, "v42bisCompressionActive" },
	{ 0x30, 0x3003, "v42bisDictionarySize" },
	{ 0x30, 0x3004, "v42bisStringLength" },
	{ 0x30, 0x300A, "modeV44" },
	{ 0x30, 0x300B, "v44CompressionActive" },
	{ 0x30, 0x300C, "v44EncoderDictionarySize" },
	{ 0x30, 0x300E, "v44EncoderStringLength" },
	{ 0x30, 0x300F, "v44DecoderDictionarySize" },
	{ 0x30, 0x3011, "v44DecoderStringLength" },
	/* 6.13 */
	{ 0x31, 0x3101, "txFlowControl" },
	{ 0x31, 0x3102, "rxFlowControl" },
	{ 0x31, 0x3103, "protocol" },
	{ 0x31, 0x3104, "txSpeed" },
	{ 0x31, 0x3105, "rxSpeed" },
	{ 0x31, 0x3106, "txThroughput" },
	{ 0x31, 0x3107, "rxThroughput" },
	{ 0x31, 0x3108, "txErrors" },
	{ 0x31, 0x3109, "rxErrors" },
};

/* the pump a modulation's high-level object is for (0: none) */
u32 v59_pump(u8 high)
{
	switch (high) {
	case 0x09: return DP_V90;
	case 0x0A: return DP_V91;
	case 0x0B: return DP_V92;
	case 0x0C: return DP_V34;
	case 0x0D: return DP_V32BIS;
	case 0x0E: return DP_V22BIS;
	case 0x0F: return DP_V23;
	case 0x10: return DP_V21;
	case 0x23: return DP_V18;
	default: return 0;
	}
}

/* pump id is registered */
bool v59_registered(u32 id)
{
	for (const DpDriver *d = dp_drivers(); d->id; d++)
		if (static_cast<u32>(d->id) == id)
			return d->op != nullptr;
	return false;
}

/* a modulation's name as V.59's modulationHistory has it (the
   Recommendation's number) */
const char *v59_rec(s16 dp)
{
	switch (dp) {
	case DP_V17: return "V.17";
	case DP_V21: return "V.21";
	case DP_V22: return "V.22";
	case DP_V22BIS: return "V.22bis";
	case DP_V23: return "V.23";
	case DP_V26: return "V.26";
	case DP_V26TER: return "V.26ter";
	case DP_V26BIS: return "V.26bis";
	case DP_V27TER: return "V.27ter";
	case DP_V29: return "V.29";
	case DP_V32: return "V.32";
	case DP_V32BIS: return "V.32bis";
	case DP_V33: return "V.33";
	case DP_V34: return "V.34";
	case DP_V90: return "V.90";
	case DP_V91: return "V.91";
	case DP_V92: return "V.92";
	case DP_V18: return "V.18";
	default: return nullptr;
	}
}

/* Table 1/V.59's code for a symbol rate, -1 for one it has none for */
int v59_symbol_code(float rate)
{
	static const int rates[] = {
		8000, 3429, 3000, 3200, 2800, 2743, 2400, 1600, 1200, 600,
	};
	long r = lrint(rate);
	for (int i = 0; i < 10; i++)
		if (r == rates[i])
			return i;
	return -1;
}

/* Table 2/V.59's code for a carrier frequency, -1 for one it has none
   for (a PCM direction's 0) */
int v59_carrier_code(float hz)
{
	static const int freqs[] = {
		1200, 1600, 1646, 1680, 1700, 1800, 1829, 1867, 1920, 1959,
		2000, 2400,
	};
	long f = lrint(hz);
	for (int i = 0; i < 12; i++)
		if (f == freqs[i])
			return i;
	return -1;
}

/* a level with its unit, one decimal */
std::string v59_dbm0(double db)
{
	return fixed(db, 1) + "dBm0";
}

const char *const V59_NULL = "NULL";

}  // namespace

/* V.59's high-level object for the last call's modulation (0x26,
   NSMDiag, for one V.59 has none for), or -1 when there was no data
   connection */
s16 Modem::v59_high_now() const
{
	if (!stats_.call || !stats_.connected || stats_.fax)
		return -1;
	switch (stats_.dp) {
	case DP_V90: return 0x09;
	case DP_V91: return 0x0A;
	case DP_V92: return 0x0B;
	case DP_V34: return 0x0C;
	case DP_V32: case DP_V32BIS: return 0x0D;
	case DP_V22: case DP_V22BIS: return 0x0E;
	case DP_V23: return 0x0F;
	case DP_V21: return 0x10;
	case DP_V18: return 0x23;
	default: return 0x26;
	}
}

/* Table 5/V.59's code for a modulation's or protocol's mode object
   (tag xx00, or 300A) */
u32 Modem::v59_mode(u16 tag) const
{
	const u16 ec = sregs_[SREG_EC], comp = sregs_[SREG_COMP];
	switch (tag) {
	case 0x2F00:
		/* \N3 (the default), \N2 and \N4 (V.42 too), \N0, \N1
		   and \N5 (none, or MNP) */
		return ec == 3 ? 1 : ec == 2 || ec == 4 ? 2 : 3;
	case 0x3000:
		return comp & 2 ? kept("+DS")[0] ? 1 : 3 : 3;
	case 0x300A:
		return comp & 2 ? kept("+DS44")[0] ? 1 : 3 : 3;
	}
	u8 high = static_cast<u8>(tag >> 8);
	/* the fax modulations (the fax engine's, class 1 and 2): there in
	   fax mode */
	if (high >= 0x15 && high <= 0x18)
		return 2;
	u32 dp = v59_pump(high);
	if (!dp || !v59_registered(dp))
		return 0;
	/* V.18: there when +MV18S asks for it */
	if (high == 0x23)
		return 2;
	/* the data modulations: +MS's the preferred one, the others
	   there with its automode */
	u32 sel = sregs_[SREG_DP];
	if (sel == dp || (dp == DP_V32BIS && sel == DP_V32) ||
	    (dp == DP_V22BIS && sel == DP_V22))
		return 1;
	return sregs_[SREG_AUTOMODE] ? 2 : 3;
}

/* an object's value (its tag; "NULL" when it does not apply to the last
   call) */
std::string Modem::v59_value(u16 tag) const
{
	const u8 high = static_cast<u8>(tag >> 8), low = tag & 0xFF;
	const struct dp_stats &d = dp_stats_;
	const bool up = stats_.call && stats_.connected && !stats_.fax;
	const bool ec = up && (cfg_.ec || cfg_.mnp_on);
	const std::string_view comp = compression_name();
	const dlink::Stats &l = link_->stats();
	auto boolean = [](bool b) { return std::string(b ? "TRUE" : "FALSE"); };
	auto num = [](u64 v, u64 most) { return dec(std::min(v, most)); };
	/* the seconds the call was up */
	auto seconds = [this]() -> double {
		u32 end = stats_.ended ? stats_.end :
			static_cast<u32>(host_.now_ms());
		return (end - stats_.up) / 1000.0;
	};
	auto history = [](const u32 *h, u8 n) {
		std::string s;
		for (u8 i = 0; i < n; i++)
			s += (i ? ";" : "") + dec(h[i]);
		return s.empty() ? std::string(V59_NULL) : s;
	};
	/* each direction's level while the call was up, dBm0 */
	auto level = [this](bool rx) {
		return 10 * log10((rx ? stats_.rx_pow : stats_.tx_pow) /
				  static_cast<double>(stats_.samples) /
				  DP_DBM0_POWER + 1e-9);
	};

	switch (tag) {
	/* 6.5: the identity; V.59's version (1); what the modem does
	   (Table 4: data, T.30's fax, its Annex F (V.34), V.253's voice,
	   V.70's DSVD, V.80, V.18) */
	case 0x0001:
		return author;
	case 0x0002:
		return product;
	case 0x0003:
		return version;
	case 0x0004:
		return "1";
	case 0x0005:
		return hex(0x01 | 0x02 | 0x08 | 0x10 | 0x20 | 0x100 | 0x200,
			   0, true);
	/* 6.6: the modes there are besides V.59's */
	case 0x0101: {
		static const struct { u32 dp; const char *name; } other[] = {
			{ DP_K56, "K56flex" }, { DP_V32TERBO, "V.32terbo" },
			{ DP_V26TER, "V.26ter" }, { DP_V26BIS, "V.26bis" },
			{ DP_B103, "Bell103" }, { DP_B212, "Bell212A" },
			{ DP_VCVM, "V.cvm" }, { DP_VWB, "V.wb" },
		};
		std::string s;
		for (const auto &o : other)
			if (v59_registered(o.dp) ||
			    (o.dp == DP_K56 && v59_registered(DP_V90)))
				s += (s.empty() ? "" : ";") +
					std::string(o.name);
		return s.empty() ? V59_NULL : s;
	}
	case 0x0102:
		return "MNP4;MNP5;zstd";
	/* 6.7: the call's modulation (the core keeps the one it came up
	   with) */
	case 0x0201: {
		const char *r = up ? v59_rec(stats_.dp) : nullptr;
		const char *n = up ? dp_name(stats_.dp) : nullptr;
		return r ? r : n ? n : V59_NULL;
	}
	/* 6.8.1: CM and JM as they went (each octet from the call
	   function's on, in hexadecimal); Table 6: a common mode, or none
	   (the call came up without V.8) */
	case 0x0702:
	case 0x0703: {
		const unsigned char *b = tag == 0x0702 ? d.v8_cm : d.v8_jm;
		int n = tag == 0x0702 ? d.v8_cm_n : d.v8_jm_n;
		if (!stats_.call || n <= 0)
			return V59_NULL;
		std::string s;
		for (int i = 0; i < n; i++)
			s += (i ? ";" : "") + hex(b[i], 2, true);
		return s;
	}
	case 0x0704:
		if (!stats_.call)
			return V59_NULL;
		if (d.v8_cm_n > 0 && d.v8_jm_n > 0)
			return up ? "0" : "1";
		return up ? "2" : V59_NULL;
	/* 6.8.18 */
	case 0x2601: {
		const char *n = up && v59_high_now() == 0x26 ?
			dp_name(stats_.dp) : nullptr;
		return n ? n : V59_NULL;
	}
	/* 6.9 (txCarrier and rxCarrier as +MS names them: V.90's and
	   V.92's analogue modem sends V.34 when its direction is not
	   PCM) */
	case 0x2C01:
	case 0x2C02: {
		const char *n = up ? dp_name(stats_.dp) : nullptr;
		if (!n)
			return V59_NULL;
		if (tag == 0x2C01 && (stats_.dp == DP_V90 ||
				      stats_.dp == DP_V92) &&
		    (d.have & DP_STATS_LINE) && lrint(d.sym_tx) != 8000)
			return "V34";
		return n;
	}
	case 0x2C03:
		return up ? num(d.carrier_losses, 255) : V59_NULL;
	case 0x2C04:
	case 0x2C05:
		return up ? num(d.renegs_local + d.renegs_remote, 255) :
			V59_NULL;
	case 0x2C06:
	case 0x2C07:
		return up ? num(d.retrains_local + d.retrains_remote, 255) :
			V59_NULL;
	case 0x2C09: {
		if (!stats_.call || !stats_.connected)
			return V59_NULL;
		u32 s = static_cast<u32>(seconds());
		return dec(s / 3600, 2) + "H" + dec(s / 60 % 60, 2) + "M" +
			dec(s % 60, 2) + "S";
	}
	case 0x2C0A:
		return stats_.call ? boolean(stats_.caller) : V59_NULL;
	/* 6.11: Table 16, the call's progress (an answer tone, ANSam
	   when V.8's CM or JM went, for a call that did not come up);
	   Table 17, why it ended (NULL while it goes on) */
	case 0x2E01:
		if (!stats_.call)
			return "0";
		if (stats_.connected)
			return "40";
		switch (stats_.cause) {
		case Cause::NoDialtone: return "1";
		case Cause::Busy: return "4";
		case Cause::TrainingFailed:
		case Cause::ProtocolFailure:
			return d.v8_cm_n > 0 || d.v8_jm_n > 0 ? "7" : "6";
		case Cause::None: return V59_NULL;
		default: return "2";
		}
	case 0x2E02:
		/* (ATZ ended it: that much kept, as 5.2.2/V.59 asks) */
		if (!stats_.call && stats_.cause == Cause::DteReset)
			return "90";
		if (!stats_.call || !stats_.ended)
			return V59_NULL;
		switch (stats_.cause) {
		case Cause::LocalHangup:
		case Cause::AnyKey:
		case Cause::DteReset: return "90";
		case Cause::DtrOff: return "32";
		case Cause::RemoteCleardown: return "65";
		case Cause::LineDropped: return "46";
		case Cause::CarrierLost: return "53";
		case Cause::NoDialtone: return "47";
		case Cause::Busy: return "51";
		case Cause::NoAnswer: return "44";
		case Cause::TrainingFailed: return "54";
		case Cause::ProtocolFailure: return "80";
		default: return "0";
		}
	/* 6.12: Table 18, the error correction; its frames (the
	   information field and the window agreed, each way); the counts
	   (16 bits, held at their most) */
	case 0x2F01:
		return !up ? V59_NULL : cfg_.ec ? "1" : cfg_.mnp_on ? "2" : "0";
	case 0x2F03:
		return !ec ? V59_NULL : dec(cfg_.ec ? link_->config().tx_info :
					    link_->config().mnp_info);
	case 0x2F04:
		return !ec ? V59_NULL : dec(cfg_.ec ? link_->config().rx_info :
					    link_->config().mnp_info);
	case 0x2F05:
		return !ec ? V59_NULL : dec(cfg_.ec ? link_->config().tx_win :
					    link_->config().mnp_k);
	case 0x2F06:
		return !ec ? V59_NULL : dec(cfg_.ec ? link_->config().rx_win :
					    link_->config().mnp_k);
	case 0x2F07:
		return ec ? num(l.timeouts, 511) : V59_NULL;
	case 0x2F08:
		return ec ? num(l.tx_frames, 65535) : V59_NULL;
	case 0x2F09:
		return ec ? num(l.tx_again, 65535) : V59_NULL;
	case 0x2F0A:
		return ec ? num(l.rx_frames, 65535) : V59_NULL;
	case 0x2F0B:
	case 0x2F0D:
		return ec ? num(l.rx_errors, 65535) : V59_NULL;
	case 0x2F0C:
		return ec ? num(l.naks, 65535) : V59_NULL;
	/* the DTE's characters each way over the time the call was up,
	   bit/s (at the error correction's input, and at the DTE's
	   interface: the same here) */
	case 0x2F0E:
	case 0x2F0F:
	case 0x3106:
	case 0x3107: {
		if (!up || (high == 0x2F && !ec))
			return V59_NULL;
		double s = seconds();
		u32 chars = (tag & 1) ? l.rx_chars : l.tx_chars;
		if (s <= 0)
			return V59_NULL;
		return num(static_cast<u64>(lrint(chars * 8 / s)), 16777215);
	}
	/* Table 20: what was agreed (D-Modem offers V.44 with V.42bis,
	   as +DS44 has it); Tables 21 and 22: each one's directions (P0:
	   V.42bis's from the initiator, the caller, V.44's from us) */
	case 0x3001:
		if (!up)
			return V59_NULL;
		if (comp == "V42bis")
			return kept("+DS44")[0] ? "2" : "1";
		if (comp == "V44")
			return kept("+DS")[0] ? "3" : "4";
		return comp == "NONE" ? "0" : "5";
	case 0x3002:
	case 0x300B: {
		if (!up)
			return V59_NULL;
		bool v44 = tag == 0x300B;
		if (comp != (v44 ? "V44" : "V42bis"))
			return "0";
		u32 rx = cfg_.comp & 1, tx = (cfg_.comp >> 1) & 1;
		if (v44 || stats_.caller)
			return dec(tx | rx << 1);
		return dec(rx | tx << 1);
	}
	case 0x3003:
	case 0x3004:
		if (!up || comp != "V42bis")
			return V59_NULL;
		return dec(tag == 0x3003 ? cfg_.dict_size : cfg_.max_string);
	case 0x300C:
	case 0x300F:
	case 0x300E:
	case 0x3011:
		if (!up || comp != "V44")
			return V59_NULL;
		return dec(tag == 0x300C || tag == 0x300F ? cfg_.dict_size :
			   cfg_.max_string);
	/* 6.13: &H's flow control each way (the DTE's 133 for what we
	   send it, our 106 for what it sends), +ICF's framing (V.80's
	   modes in its place), +IPR's rate (NULL: the DTE's own, found
	   from what it sends), the characters lost each way (16 bits) */
	case 0x3101:
	case 0x3102: {
		u16 h = sregs_[SREG_FLOW_CONTROL];
		std::string s;
		if (h == 1 || h == 3)
			s = tag == 0x3101 ? "133" : "106";
		if (h == 2 || h == 3)
			s += (s.empty() ? "" : ";") + std::string("XON/XOFF");
		return s.empty() ? "None" : s;
	}
	case 0x3103: {
		if (up && link_->sync())
			return link_->sync_framed() ? "V.80 framed" :
				"V.80 transparent";
		static const char *const fmt[] = {
			"8N2", "8?1", "8N1", "7N2", "7?1", "7N1",
		};
		static const char par[] = { 'O', 'E', 'M', 'S' };
		const u32 *icf = kept("+ICF");
		if (icf[0] < 1 || icf[0] > 6)
			return "Auto";
		std::string s = fmt[icf[0] - 1];
		if (s[1] == '?')
			s[1] = par[icf[1] & 3];
		return s;
	}
	case 0x3104:
	case 0x3105: {
		u32 r = kept("+IPR")[0];
		return r ? dec(r) : V59_NULL;
	}
	case 0x3108:
		return up ? num(stats_.tx_lost, 65535) : V59_NULL;
	case 0x3109:
		return up ? num(l.rx_lost, 65535) : V59_NULL;
	}

	/* the mode objects (Table 5) */
	if (low == 0x00 || tag == 0x300A)
		return dec(v59_mode(tag));
	if (tag == 0x0B21)
		return boolean(moh_off_ == 0);

	/* the modulations' objects: NULL but for the last call's */
	if (static_cast<s16>(high) != v59_high_now())
		return V59_NULL;
	const bool line = d.have & DP_STATS_LINE;
	const bool pcm = d.have & DP_STATS_PCM;
	switch (low) {
	case 0x47:
		return stats_.samples ? v59_dbm0(level(false)) : V59_NULL;
	case 0x48:
	case 0x49: {
		int c = line ? v59_symbol_code(low == 0x48 ? d.sym_tx :
					       d.sym_rx) : -1;
		return c < 0 ? V59_NULL : dec(c);
	}
	case 0x4A:
	case 0x4B: {
		int c = line ? v59_carrier_code(low == 0x4A ? d.carrier_tx :
						d.carrier_rx) : -1;
		return c < 0 ? V59_NULL : dec(c);
	}
	case 0x4C:
		/* (V.22bis's dataRateHistory: one rate both ways) */
		return history(stats_.tx_hist, stats_.tx_hist_n);
	case 0x4D:
		return history(stats_.rx_hist, stats_.rx_hist_n);
	case 0x4E:
		return stats_.samples ? v59_dbm0(level(true)) : V59_NULL;
	case 0x4F:
		/* the receiver's noise: its level less its SNR */
		return stats_.samples && (d.have & DP_STATS_SNR) ?
			v59_dbm0(level(true) - d.snr) : V59_NULL;
	case 0x51:
		return d.have & DP_STATS_NEAR_ECHO ? v59_dbm0(d.near_echo) :
			V59_NULL;
	case 0x52:
		return d.have & DP_STATS_FAR_ECHO ? v59_dbm0(d.far_echo) :
			V59_NULL;
	case 0x53:
		return d.have & DP_STATS_RTD ? dec(d.rtd) + "ms" : V59_NULL;
	case 0x55:
		/* Table 3: bit n, the frame interval n + 1 of 6 robbed */
		return d.have & DP_STATS_DIL ? hex(static_cast<u32>(
			d.robbed_bits & 0x3F), 0, true) : V59_NULL;
	case 0x56:
		return d.have & DP_STATS_DIL ? fixed(d.digital_loss, 1) + "dB" :
			V59_NULL;
	}
	switch (tag) {
	/* V.90's codec (TRUE A-law): the network's; V.91's and V.92's
	   each end's (a single law on a call) */
	case 0x0907:
	case 0x0A04:
	case 0x0A05:
	case 0x0B07:
	case 0x0B08:
		return pcm ? boolean(d.pcm_alaw) : V59_NULL;
	case 0x0B48: {
		int c = line ? v59_symbol_code(d.sym_tx) : -1;
		return c < 0 ? V59_NULL : dec(c);
	}
	/* V.23: duplex (TRUE), and Table 13's rate each way */
	case 0x0F01:
		return boolean(true);
	case 0x0F02:
	case 0x0F03: {
		u32 r = tag == 0x0F02 ? (stats_.ended ? stats_.tx1 : tx_rate_) :
			(stats_.ended ? stats_.rx1 : rx_rate_);
		return r == 1200 ? "0" : r == 600 ? "1" : r == 75 ? "2" :
			V59_NULL;
	}
	}
	return V59_NULL;
}

/* +TMO (V.250 6.9): V.59's objects (see the top of this file); the
   command takes the line up to a ";" */
s16 Modem::at_TMO(char *p, s16 *len)
{
	const char *end = strchr(p + 4, ';');
	std::string arg(p + 4, end ? static_cast<size_t>(end - p - 4) :
			strlen(p + 4));
	*len = static_cast<s16>(4 + arg.size());
	if (arg.empty()) {
		if (last_tmo_.empty())
			return -1;
		arg = last_tmo_;
	}
	std::string a;
	for (char c : arg)
		a += upper_ascii(c);

	auto line = [this](const std::string &s) {
		puts(s);
		crlf();
	};
	auto high_tag = [](u8 t) { return hex(t, 2, true); };
	auto tag4 = [](u16 t) { return hex(t, 4, true); };

	/* <level>[N]=? */
	if (a.size() >= 2 && a.compare(a.size() - 2, 2, "=?") == 0) {
		std::string lv = a.substr(0, a.size() - 2);
		bool names = !lv.empty() && lv.back() == 'N';
		if (names)
			lv.pop_back();
		if (lv.size() > 1 || (lv.size() == 1 && (lv[0] < '0' ||
							   lv[0] > '4')))
			return -1;
		int level = lv.empty() ? 0 : lv[0] - '0';
		if (level == 4) {
			if (names)
				return -1;
			line("+TMO: (0,1)");
			last_tmo_ = arg;
			return 0;
		}
		std::string list;
		auto add = [&list](const std::string &s) {
			list += (list.empty() ? "" : ",") + s;
		};
		if (level == 0 || level == 1)
			for (const auto &h : v59_high)
				add(names ? h.name : high_tag(h.tag));
		if (level != 1) {
			/* each object once (ModemCapability's are the
			   modulations' too): the mid-level ones, txPower
			   (of TxPowerLevel) the low-level */
			for (const auto &o : v59_objs) {
				if (o.high == 0x01 && o.tag >> 8 != 0x01)
					continue;
				bool lowlevel = (o.tag & 0xFF) == 0x47;
				if ((level == 2 && lowlevel) ||
				    (level == 3 && !lowlevel))
					continue;
				add(names ? o.name : tag4(o.tag));
			}
		}
		line("+TMO: (" + list + ")");
		last_tmo_ = arg;
		return 0;
	}

	/* "ALL" after a high-level object: the same as none */
	auto strip_all = [](std::string &s) {
		if (s.size() > 3 && s.compare(s.size() - 3, 3, "ALL") == 0)
			s.resize(s.size() - 3);
	};
	/* every object of a high-level one, or one */
	auto show_high = [&](u8 high, bool names) {
		for (const auto &o : v59_objs)
			if (o.high == high)
				line((names ? std::string(o.name) : tag4(o.tag)) +
				     "{" + v59_value(o.tag) + "}");
	};
	auto show = [&](const V59Obj &o, bool names) {
		line((names ? std::string(o.name) : tag4(o.tag)) + "{" +
		     v59_value(o.tag) + "}");
	};

	/* a tag: four digits one object, two its high-level object's */
	if (a[0] >= '0' && a[0] <= '9') {
		strip_all(a);
		u32 t = 0;
		for (char c : a) {
			int v = c >= '0' && c <= '9' ? c - '0' :
				c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
			if (v < 0)
				return -1;
			t = t << 4 | static_cast<u32>(v);
		}
		if (a.size() == 2) {
			for (const auto &h : v59_high)
				if (h.tag == t) {
					show_high(h.tag, false);
					last_tmo_ = arg;
					return 0;
				}
			return -1;
		}
		if (a.size() != 4)
			return -1;
		for (const auto &o : v59_objs)
			if (o.tag == t) {
				show(o, false);
				last_tmo_ = arg;
				return 0;
			}
		return -1;
	}

	/* an object by its name alone: the last call's modulation's when
	   there are several */
	auto find_obj = [&](const std::string &name, s16 high) -> const V59Obj * {
		const V59Obj *first = nullptr;
		for (const auto &o : v59_objs) {
			if (!same_nocase(o.name, name))
				continue;
			if (high >= 0) {
				if (o.high == high)
					return &o;
				continue;
			}
			if (o.high == v59_high_now())
				return &o;
			if (!first && o.high != 0x01)
				first = &o;
		}
		return first;
	};
	if (const V59Obj *o = find_obj(a, -1)) {
		show(*o, true);
		last_tmo_ = arg;
		return 0;
	}
	/* a high-level object (All), or one of its objects after it */
	const V59High *best = nullptr;
	size_t best_n = 0;
	for (const auto &h : v59_high)
		for (const char *n : { h.name, h.alias }) {
			if (!n)
				continue;
			size_t k = strlen(n);
			if (k > best_n && starts_nocase(a, n)) {
				std::string rest = a.substr(k);
				if (rest.empty() || rest == "ALL" ||
				    find_obj(rest, h.tag)) {
					best = &h;
					best_n = k;
				}
			}
		}
	if (!best)
		return -1;
	std::string rest = a.substr(best_n);
	if (rest.empty() || rest == "ALL")
		show_high(best->tag, true);
	else
		show(*find_obj(rest, best->tag), true);
	last_tmo_ = arg;
	return 0;
}

}  // namespace dcore
