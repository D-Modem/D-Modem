# ZyXEL security

Status: type 2 (`AT*G3` to `AT*G5`) is in D-Modem. Type 1 (`AT*G1`,
`AT*G2`) is not.

## What it is

ZyXEL modems (the U-1496 family and the Elite 2864) can ask the caller
for a password before the data passes. They can also hang up and call
back. Type 2 is plain text on the data link, thus a caller with any
modem and terminal can use it. Type 1 is an exchange between two ZyXEL
modems. The user does not see it.

The design comes from the U-1496S 5.06B firmware. The trace notes have
the addresses.

## How to use it

On the modem that answers:

```
AT*H0="abc"*H1="Secret"
AT&Z1="5551234"
AT*G4
```

| Command | Function |
|---|---|
| `AT*G0` | No security (the default). |
| `AT*G3` | The password, then the data. |
| `AT*G4` | The password, then a call back to the stored number of the password's slot. |
| `AT*G5` | The password, then a call back to a number that the caller types. |
| `AT*Hn=pw` | User password n (0 to 9). Up to 8 characters, with case, no comma. An empty value empties the slot. |
| `AT*HS=pw` | The supervisory password. When it is set, `*Hn=`, `*HS=` and `*V` need it first: `AT*H2=super,pw`, `AT*V=super`. |
| `AT*V` | The table: `*G`, then each slot in use, its password and its stored number. |
| `AT&Zn=number` | Stored number n (0 to 9). `ATDS=n` or `ATDSn` dials it. |

`AT*H0` to `AT*H2` without `=` stay the MNP 10 command. The settings
are not stored in the modem. Put them in the init string. `ATZ` keeps
them.

## What the caller sees

```
CONNECT 14400
PassWord:***
Error..., try again
PassWord:******
Correct
```

1. After CONNECT, the answerer sends `PassWord:`.
2. Each character comes back as `*`. CR (S3) ends the password.
3. A wrong password sends `Error..., try again` and the prompt again. A
   9th character before CR is a wrong password. The third wrong password
   hangs up. The cause (`ATI6`) is `Security check`.
4. With `*G3`, `Correct` follows, then the data.
5. With `*G4`, `Correct` follows, then the answerer hangs up. 2 seconds
   later it calls the stored number of the slot (`AT&Zn=`). If the slot
   has no number, it only hangs up.
6. With `*G5`, `Call back phone number:` follows (no `Correct`). The
   caller types up to 26 characters. Backspace (S5) erases one. A 27th
   character is a wrong password. At CR the answerer hangs up and calls
   the number 2 seconds later.

The local DTE of the answerer gets CONNECT at the usual time. It gets no
data from the caller until the password is correct. The data from the
local DTE waits in the buffer. During a call back, the local DTE gets NO
CARRIER, then the result of the new call (CONNECT). `ATH` or `ATZ` stops
a call back that is due.

## What is not the same as the firmware

| Item | ZyXEL | D-Modem |
|---|---|---|
| Time to the call back | From an internal setting (`$A1B` - 1), not known | 2 seconds |
| `*Hn` | Asks for the password on the DTE (`PassWord:`, `Verify :`) | `*Hn=pw` on the command line |
| Storage | NVRAM | The init string |
| Type 1 | ZyXEL to ZyXEL, in an HDLC message | Not done: the frame format is not known |

## Type 1

The caller sends `S`, `C` and its 8-byte password. The answerer compares
it with the 10 slots and sends `SOK`. The same channel carries remote
configuration (`SR`, `SW`, `*F1`). The firmware sends these messages on
an HDLC channel of the 68302. The flags, the address and the CRC come
from settings that the code does not show. A recording of two ZyXEL
modems is necessary to make type 1.

## Tests

- `core_test` (`Core.ZyxelSecurity`): the limits (8 characters, 26 digits,
  3 tries), the empty slot, backspace.
- `core/test/scripts/security.txt`: two modems back to back. `*G3` with a
  wrong and a correct password, three wrong passwords, the call back with
  `*G4` and with `*G5`, and the supervisory password.
