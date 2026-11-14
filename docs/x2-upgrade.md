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

On the host:

- D-Modem on a line that can answer the call, for example
  [app_dmodem](asterisk.md) or [dmodem-ari](ari.md).
- The program `tools/x2upgrade-host.py`. It shows the caller the
  result. It needs Python 3 and no other packages.

## Set up the host

Make a profile for the host in `dmodem.conf`:

    [x2upgrade]
    mode = exec
    init = AT+X2UP=2
    command = exec /usr/bin/python3 /opt/d-modem/tools/x2upgrade-host.py --log /var/log/x2upgrade.log

Then send a number to the profile in `extensions.conf`:

    exten => 5553000,1,DModem(x2upgrade)
     same => n,Hangup()

The exchange uses only the error-corrected link (LAPM). Thus it does
not need x2 or a digital line. But the tests used x2 calls only.

## AT+X2UP

`AT+X2UP=<mode>` sets what the modem does after LAPM connects.

| Mode | Function |
| --- | --- |
| 0 | Do nothing (the default). |
| 1 | Read the serial number and the feature records. Make no change. |
| 2 | Read them. If x2 is disabled, enable it. Then read the records again. |

When the exchange is complete, the modem sends one line to the DTE:

    +X2UP: <result>,"<serial number>",<features before>,<features after>

The features are the value that the Courier shows in the fifth field of
`ATY14`, in decimal. The value 16 is the x2 bit. For example, 015
becomes 031.

| Result | Meaning |
| --- | --- |
| `ENABLED` | x2 was disabled. Now it is enabled. |
| `ALREADY` | x2 was enabled before the call. The host made no change. |
| `REFUSED` | The host sent the change, but the records did not change. Set `ATS30=1` on the Courier and call again. |
| `QUERIED` | Mode 1. The host read the records and made no change. |
| `NO REPLY` | The remote modem did not reply. It is not a Courier, or it has no LAPM. |

## What the caller does

1. Set the modem to its factory settings: `AT&F1`.
2. If the supervisor is later than 7.1.4, type `ATS30=1`.
3. Dial the host. Wait for the result. The host then hangs up.
4. Type `AT&F1S0=0&W`.
5. Set the power switch of the modem to OFF, then to ON.
6. Type `ATI7`. Make sure that `x2` is in the Options line.

The change has no effect until step 5. The Courier reads its feature
records only when it starts.

## Limits

- The host changes only one bit: x2 (value 16) in record 2. It does not
  change the country or other features.
- In supervisor builds that have V.90, the same bit enables V.90.
- The tests used emulated Couriers (courier-emu), not a physical
  modem. See [the technical details](x2-upgrade-details.md#tests).
