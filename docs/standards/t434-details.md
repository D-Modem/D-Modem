# T.434: technical details

This page gives the technical details of T.434. For the summary, refer
to [T.434](t434.md).

## The message (Annex A)

`BINARY-DATA-Message ::= [APPLICATION 23] IMPLICIT SEQUENCE OF
SEQUENCE { the attributes }`. The module has no tag default, so a tag
is EXPLICIT when the attribute does not say IMPLICIT.

| Attribute | Tag | Type |
|---|---|---|
| protocol-version | [28] | BIT STRING { version-3 (2) }. Version 1 if it is not there. |
| filename | [0] IMPLICIT | SEQUENCE OF UTF8String: the name, then parts of the prefix |
| contents-type | [2] | The document type (default: unstructured binary) |
| date-and-time-of-creation, -last-modification, -last-read-access | [4], [5], [6] IMPLICIT | GeneralizedTime |
| filesize, future-filesize | [13], [14] IMPLICIT | INTEGER |
| data-file-content | [30] | CHOICE { EXTERNAL, OCTET STRING } |
| mime-media-type | [32] | SEQUENCE { media-type IA5String, parameter SEQUENCE OF IA5String OPTIONAL } |

5.19 defines the MIME type as a SEQUENCE. Annex A tags the type itself
`[32] EXPLICIT`, so the field [32] can contain a second [32] before the
SEQUENCE.

## BER

A BER element has an identifier (class, constructed, tag), a length
and the contents. A length is definite (short or long form) or
indefinite (ended by two 0 octets). A string can be constructed from
pieces.

## T.434 in D-Modem

| Item | D-Modem |
|---|---|
| Encoder (`dsp/t434.cpp`) | For each file: protocol-version 3, the name (and the prefix), the date of the last change, the size, the MIME type, and the data as an OCTET STRING. Definite lengths. |
| Decoder | Any order of the attributes. Definite and indefinite lengths. Constructed strings. The data as an OCTET STRING or as EXTERNAL. The MIME type with one [32] or two. The other attributes are skipped. |
| T.30 (`dsp/t30.cpp`) | DIS/DCS bit 53. The message goes in ECM, as it is. |

The tests `T434.*` (`dsp/test/t434_test.cpp`) check these items:

- The octets of the message, and the round trip.
- Messages from other coders: indefinite lengths, strings in pieces,
  EXTERNAL.
- The length of a message that is not complete, and damaged data.

## Corrections (2026-11)

| Error | Correction |
|---|---|
| The decoder found no MIME type when the field had the second [32] of Annex A | The decoder accepts the MIME type with one [32] or two |

The test `T434.MimeTaggedTwice` (`dsp/test/t434_test.cpp`) checks this
correction.
