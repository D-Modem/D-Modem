# Hayes V-series protocols

The Hayes V-series Smartmodem 9600 (1988) has protocols of its own that
Hayes did not publish. We read them from the firmware of the modem
(version 1.4). D-Modem now has these parts:

| Part | What it is | State |
|---|---|---|
| **Hayes ADC** | the compression of Hayes for V.42 calls to a modem without V.42bis | Done. The output agrees with the firmware, byte for byte. |
| **The Hayes XID sets** | the "AFT" and "HMP" parameter sets that a Hayes modem adds to the V.42 XID | Done. `ATS46=138` sends them. |
| **AFT** | V.42 frames on start-stop characters | The framer is done. It is not connected to a call yet. |
| **Express 96 link rules** | the bursts of the half-duplex Express 96 link | Done and tested in loop-back only |
| **Express 96 modulation** | the 9600 bit/s half-duplex carrier | Blocked. It is in the ROM of the data pump, which we do not have. |

To let a V.42 call use the Hayes sets and ADC, set `ATS46=138`. To send
the sets without compression, set `ATS46=136`. The default is `ATS46=0`
(no Hayes sets). With a Hayes modem that does not have V.42bis, the
call then uses ADC. CONNECT shows `COMPRESSION: ADC` (result code 68).

For the details of each protocol, its open items and its tests, refer to
[the technical details](hayes-details.md).
