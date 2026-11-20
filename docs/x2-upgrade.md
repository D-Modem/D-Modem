# The x2 upgrade host

Written in ASD-STE100 Simplified Technical English.

## What it does

In 1997, x2 was an option at extra cost on a U.S. Robotics Courier
V.Everything. The owner did three steps to get x2:

1. Load the x2 code into the modem.
2. Call a U.S. Robotics host. The host enables x2 in the modem.
3. Set the power switch of the modem to OFF, then to ON.

The U.S. Robotics host is not available now. D-Modem can do its work.
A Courier calls D-Modem, and D-Modem enables the x2 feature of that
Courier.

U.S. Robotics did not publish how the host operates. The method comes
from the Courier firmware, which contains the modem side of the
exchange. [The technical details](x2-upgrade-details.md) give the
messages.

## What is necessary

On the Courier:

- The x2 code. The supervisor must be 7.0.0 or later. Use `ATI7` to
  see the supervisor revision.
- V.42 error control (LAPM). The factory settings (`AT&F1`) enable it.
- Supervisor builds after 7.1.4 also need `ATS30=1`. This bit is
  "Enable feature upgrades". Without it, the modem does not keep the
  change. Supervisor 7.1.4 does not examine this bit.

Which Couriers need the host:

| Supervisor | Date | Result of the host call |
| --- | --- | --- |
| 6.x | 1994 to 1996 | No effect. The firmware has no upgrade code. Load 7.x first. |
| 7.1.4 to 7.3.x | 1997 to 03/98 | x2 (and V.90 in 7.3.x) operates after the next power on. |
| 7.4.16 and later | 04/98 and later | No effect on the modem. These builds always enable x2 and V.90. The host changes only the record. |

The audit of the firmware is in courier-emu's
`docs/feature-bits-audit.md`.

On the host:

- D-Modem on a line that can answer the call, for example
  [app_dmodem](asterisk.md) or [dmodem-ari](ari.md).
- A program that shows the caller a menu of the features and the
  result. There are two versions of this program. They do the same
  work. Use one of them:
    - `tools/x2upgrade-host.py`. It needs Python 3 and no other
      packages.
    - `tools/x2upgrade-host/`, in Go. It needs Go 1.22 or later to
      build, and no other packages. It is one file that has no
      dependencies when it runs.

## Set up the host

Make a profile for the host in `dmodem.conf`:

    [x2upgrade]
    mode = exec
    init = AT+X2UP=1
    command = exec /usr/bin/python3 /opt/d-modem/tools/x2upgrade-host.py --log /var/log/x2upgrade.log

To use the Go version, build it:

    cd tools/x2upgrade-host
    go build -o /opt/d-modem/bin/x2upgrade-host .

Then, in the profile, use this `command` line. The options are the
same as the options of the Python version:

    command = exec /opt/d-modem/bin/x2upgrade-host --log /var/log/x2upgrade.log

Then send a number to the profile in `extensions.conf`:

    exten => 5553000,1,DModem(x2upgrade)
     same => n,Hangup()

The options of the program:

| Option | Function |
| --- | --- |
| `--timeout S` | The time in seconds to wait for each `+X2UP:` line. The default is 60. |
| `--idle S` | The time in seconds to wait for a key in the menu. The default is 300. After this time, the program stops and makes no change. |
| `--log FILE` | Add one line for each call to `FILE`. |

### How the program operates

1. When LAPM connects, the modem reads the Courier (`AT+X2UP=1`). It
   sends `+X2UP: QUERIED,...` to the program.
2. The program shows the serial number and the features of the
   Courier. Then it shows a menu:

        Features of your modem:
          1. Terbo      ON
          2. V.FC/V.34  ON
          3. x2         ON   (changed)

        Type 1, 2 or 3 to switch a feature on or off.
        Type A to apply the changes. Type Q to stop with no change.
        >

3. The caller pushes 1, 2 or 3 to switch a feature on or off. Then the
   caller pushes A.
4. The program sends `X2UP=2,<enable>,<disable>` on its control channel
   (`DMODEM_CONTROL`, file descriptor 3). The modem does the change, and
   sends the result line to the program.
5. The program shows the result and the steps that follow. Then it
   stops, and the call ends.

The control channel is available only in `mode = exec` profiles of
app_dmodem, dmodem-ari and mod_dmodem. See
[the technical details of app_dmodem](asterisk-details.md).

### A host with no menu

Without the control channel, the program shows only the result. Then
the profile must do the change at the start of the call. Use
`init = AT+X2UP=2` (enable x2), or give the features. For example,
`init = AT+X2UP=2,0,16` makes a host that disables x2.

A program that uses libdmodem directly can start the change with
`dmodem_x2up()` (`frontend/dmodemd/dmodem.h`).

The exchange uses only the error-corrected link (LAPM). Thus it does
not need x2 or a digital line. But the tests used x2 calls only.

## AT+X2UP

`AT+X2UP=<mode>,<enable>,<disable>` sets what the modem does after
LAPM connects. During a call, the control channel command `X2UP=` and
`dmodem_x2up()` start the same exchange with the same values.

| Mode | Function |
| --- | --- |
| 0 | Do nothing (the default). |
| 1 | Read the serial number and the feature records. Make no change. |
| 2 | Read them. Enable the features in `<enable>` and disable the features in `<disable>`. Then read the records again. |

`<enable>` and `<disable>` are sums of these feature values:

| Value | Feature |
| --- | --- |
| 4 | Terbo (V.32terbo) |
| 8 | V.FC and V.34 |
| 16 | x2 (and V.90 in the supervisor builds that have it) |

The default of `<enable>` is 16, and the default of `<disable>` is 0.
Thus `AT+X2UP=2` enables x2. A feature cannot be in the two values: the
modem then gives `ERROR`. The host does not change HST (1) or fax (2).
A value that you do not give keeps its last setting. Give the two values
when you change one of them.

Examples:

| Command | Function |
| --- | --- |
| `AT+X2UP=2` | Enable x2. |
| `AT+X2UP=2,28,0` | Enable Terbo, V.FC and x2. |
| `AT+X2UP=2,0,16` | Disable x2. |
| `AT+X2UP=2,4,24` | Enable Terbo. Disable V.FC and x2. |

The effect of each feature on the Courier is in courier-emu's
`docs/feature-bits-audit.md`. On the Courier, V.FC also shows Terbo in
`ATI7`. `V34+` shows only when Terbo and V.FC are both enabled.

When the exchange is complete, the modem sends one line to the DTE:

    +X2UP: <result>,"<serial number>",<features before>,<features after>

The features are the value that the Courier shows in the fifth field of
`ATY14`, in decimal. For example, when x2 (16) is enabled, 015 becomes
031.

| Result | Meaning |
| --- | --- |
| `ENABLED` | The host enabled one or more features. |
| `DISABLED` | The host disabled one or more features. |
| `CHANGED` | The host enabled some features and disabled others. |
| `ALREADY` | The features were already as asked. The host made no change. |
| `REFUSED` | The host sent the change, but the records did not change. Set `ATS30=1` on the Courier and call again. |
| `QUERIED` | Mode 1. The host read the records and made no change. |
| `NO REPLY` | The remote modem did not reply. It is not a Courier, or it has no LAPM. |

## What the caller does

1. Set the modem to its factory settings: `AT&F1`.
2. If the supervisor is later than 7.1.4, type `ATS30=1`.
3. Dial the host. Wait for the menu.
4. Push 1, 2 or 3 to switch Terbo, V.FC or x2 on or off. Then push A.
   Wait for the result. The host then hangs up.
5. Type `AT&F1S0=0&W`.
6. Set the power switch of the modem to OFF, then to ON.
7. Type `ATI7`. Make sure that the Options line shows the change.

The change has no effect until step 6. The Courier reads its feature
records only when it starts.

## Limits

- The host changes only three bits of record 2: Terbo (4), V.FC (8) and
  x2 (16). It does not change the country, HST or fax.
- In supervisor builds that have V.90, the same bit enables V.90.
- In supervisor 7.4.16 and later, the x2 bit has no effect. The firmware
  enables x2 and V.90 at each start.
- The tests used emulated Couriers (courier-emu), not a physical
  modem. See [the technical details](x2-upgrade-details.md#6-tests).
