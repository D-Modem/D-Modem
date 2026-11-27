# FreePBX: the D-Modem module: technical details

This page gives the technical details for [FreePBX: the D-Modem module](freepbx.md), the plain-English overview. Read the overview first. This page keeps the full details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

`frontend/freepbx/dmodem/` is a FreePBX module (version 16.0.9, for FreePBX 16
and 17) for [app_dmodem](asterisk.md) or [dmodem-ari](ari.md). It
gives these items:

- the profiles of the modem, which you edit in the web interface of
  FreePBX. Each profile is a destination for inbound routes, IVRs, time conditions and similar items;
- calls out to a remote modem with a profile;
- fax recipients: destinations that receive a fax and send it by email
  (a PDF file, the TIFF file or both);
- fax detection on inbound routes (a "D-Modem fax" tab);
- a fax log and a page to send faxes;
- a Fax widget in UCP (the User Control Panel), with its rights in
  User Manager;
- the settings and profiles in the Backup & Restore of FreePBX;
- a check of the PJSIP trunks and extensions for modem calls.

It adds **Applications > D-Modem** to the menu and the `fwconsole dmodem`
command. The module is only PHP. Nothing in it is compiled.

## Installation

1. **The modem.** Select one engine:
   - **dmodem-ari** together with Asterisk (`build/ari/dmodem-ari`), with an ARI
     (Asterisk REST Interface) user in Settings > Asterisk REST
     Interface Users. This is the default engine.
   - **app_dmodem**, on an Asterisk for which you build it yourself. With
     the headers of Asterisk installed, run
     `cmake -B build -DDMODEM_APP_DMODEM=ON && cmake --build build`. Copy
     `app_dmodem.so` to the modules directory of Asterisk, then run
     `asterisk -rx "module load app_dmodem.so"`. Do not give a built app_dmodem
     to other persons ([LICENSING.md](../LICENSING.md)).
2. **The module.** `cmake --build build --target freepbx_module` makes
   `build/freepbx/dmodem-<version>.tgz`. Upload it in Admin > Module
   Admin (Upload modules). Or, extract it into the modules directory of
   FreePBX and install it from the command line:

   ```
   tar xzf dmodem-16.0.9.tgz -C /var/www/html/admin/modules/
   fwconsole ma installlocal dmodem
   fwconsole reload
   ```

   The module is not signed. Thus, FreePBX shows it as unsigned.
3. **Ghostscript**, to send faxes from the Send fax tab or with
   `fwconsole dmodem sendfax`: `dnf install ghostscript` or
   `apt install ghostscript`. Received faxes do not need it.
4. **LibreOffice** (optional), to send Office documents:
   `dnf install libreoffice-headless` or `apt install
   libreoffice-core-nogui libreoffice-writer-nogui` (and the Calc and
   Impress parts for spreadsheets and presentations).
5. **Email to fax** (optional): refer to [Email to fax](#email-to-fax).

## Use

The page of the module has eight parts.

### Profiles

A profile tells where the data of a call goes after the modems connect:

- a TCP server (a BBS);
- a server on a unix socket;
- a program on a pty (`pppd` for dial-up IP, a login, a door).

Each profile also has:

| field | default | notes |
|---|---|---|
| G.711 law | `auto` | `auto`, `ulaw` or `alaw`. |
| ISDN data calls (`clear`) | `auto` | `auto`: a clear channel for a call with a digital bearer; `yes`: every call; `no`: never. A CLEARMODE call is always clear. Refer to [isdn.md](isdn.md). |
| Modem commands (`init`) | empty | More AT commands. They start with `AT`. 200 characters at most. |
| Answer command (`answer`) | empty (`ATA`) | It starts with `AT`. 200 characters at most. For example, `AT+MS=132;A` answers at V.32bis maximum. The module writes the `;` as `\;` in `dmodem.conf`. |
| Connect timeout | 60 s | 5 to 600 s. |
| Take fax calls (`fax`) | on | Receive a calling fax machine as a fax. |
| Voice extension (`voice`) | empty | An extension, or `extension@context` (`from-internal` if there is no context). A person who calls goes to it. Empty: the modem answers all calls. |
| Listen for a person (`voice_listen`) | 3 s | 0 to 30 s. |
| Modem voice, DSVD and ASVD (`voice_dsvd`) | `no` | `no`, `yes` (to the voice extension when the caller opens the voice channel) or `open` (our modem opens it immediately). `yes` and `open` need a voice extension. |
| Call details first (`header`) | off | TCP and unix-socket servers only. |
| Hand over the line (`handoff`) | off | Only for unix-socket servers with call details first. The server gets the line of the modem (a terminal) instead of a copy of its bytes. Refer to [the handoff](asterisk-details.md#what-the-server-or-program-sees). |
| Description | empty | One line, 100 characters at most. The destination shows it. |

A profile name has 1 to 40 characters (lower-case letters, digits, `-`
and `_`). The first character is a letter or digit. The names `general`,
`receivefax`, `sendfax` and `faxdetect` are reserved. A name cannot
start with `fax-`, because the fax recipients use those names.

A profile `bbs` is the destination `app-dmodem,bbs,1`. Each destination
picker shows it under "D-Modem". After you save a profile, you cannot
change its name, because the name is its destination. You cannot delete
a profile if a route or a different object still refers to it.

### Settings

- **Engine**: the `Stasis()` of dmodem-ari (the default), or `DModem()` in an
  Asterisk that has app_dmodem built for it.
- **Log level**: the `debug` of the modem, 0 to 3.
- **dmodem-ari** (engine `ari` only): the ARI URL
  (`http://127.0.0.1:8088` by default), user (`dmodem`), password
  (empty on the form; the module keeps the stored password) and Stasis app name
  (`dmodem`). Optionally, T.38 through the SIP side of dmodem-ari, with
  its PJSIP endpoint (`dmodem`) and SIP address (`127.0.0.1:5090`).
- **Dial out**: the dial-out context (`from-internal`), the caller ID,
  and the ring time (60 s; 5 to 300 s).
- **Fax**, the fax settings of `[general]`:
  - the directory for received faxes (`/var/spool/asterisk/fax`);
  - **Keep faxes** (0): the number of days that a received fax stays
    in the [fax log](#faxes) and in the fax directory, 0 to 3650. 0
    keeps all faxes;
  - **Block faxes from**, **Never block faxes from** and **Only the
    allow list**: the [junk fax control](#junk-fax-control);
  - our fax number (up to 20 digits, `+` and spaces);
  - the fax header (up to 63 characters): the text of the header line
    on the top of each page that D-Modem sends. The line also has the
    date, the time, our fax number and the page number. Empty: no
    header line;
  - the fastest rate (14400, 12000, 9600, 7200, 4800 or 2400 bit/s);
  - the slowest rate (none, or one of the rates; not more than the
    fastest rate). If the fax cannot go at this rate or faster, the fax
    stops. `fax_min_rate` in `dmodem.conf`;
  - error correction mode (on) and T.38 (on);
  - **Fax destination** (on): a destination "D-Modem: receive a fax"
    (`app-dmodem,receivefax,1`) that answers as a fax machine.
- **Sending faxes**, for the [Send fax](#send-fax) tab:
  - **Tries again** (1): the tries after the first, 0 to 3;
  - **Minutes between tries** (5): 1 to 120;
  - **Email the result** (`failure`): `never`, `always`, `success` or
    `failure`, and **Result to** (empty): the address. These are the
    defaults of the send page;
  - **Ghostscript** (`/usr/bin/gs`): the program that makes the fax
    pages from PDF files;
  - **LibreOffice** (`/usr/bin/soffice`): the program that makes PDF
    files from Office documents. Empty: only PDF and TIFF files;
  - **Send API** (off) and **Send API token**: the
    [send API](#send-api). An empty token field keeps the token;
- **Email to fax**: refer to [Email to fax](#email-to-fax);
  - **Cover page**: the company name, the address (5 lines at most),
    the telephone, the web site and the footer.
- **Fax to email**, for the [fax recipients](#fax-recipients):
  - **Attach** (`pdf`): `pdf`, `tiff` or `both`. A recipient can have
    its own value;
  - **PDF paper size** (`letter`): `letter` or `a4`;
  - **From** (empty): an address, or `Name <address>`. Empty: the
    sender that the mail system of the machine gives;
  - **Subject** (`Fax from {cid} ({pages} pages)`): one line, 200
    characters at most;
  - **Text** (two lines): 4000 characters at most.

  In the subject and the text, the module replaces these fields:

  | field | value |
  |---|---|
  | `{cid}` | the caller ID number |
  | `{cidname}` | the caller ID name |
  | `{remote}` | the fax number of the other machine (its TSI) |
  | `{pages}` | the number of pages |
  | `{status}` | `SUCCESS` or `FAILED` |
  | `{date}` | the date and time of the fax file (`Y-m-d H:i`) |
  | `{recipient}` | the name of the recipient |
  | `{file}` | the name of the fax file |

### Fax recipients

A fax recipient is a destination that receives a fax and sends it by
email when the call ends. This is the fax-to-email function of the Fax
module of FreePBX. It operates with the two engines.

| field | notes |
|---|---|
| Id | 1 to 32 characters: lower-case letters, digits and `_`. The first character is a letter or digit. The destination is `app-dmodem,fax-<id>,1`. You cannot change the id after you save the recipient. |
| Name | One line, 100 characters at most. The destination shows "Fax to <name>". The message has it as `{recipient}`. |
| Email address | One address. If it is empty, the module uses the address of the User Manager user. |
| User Manager user | Only if User Manager is installed. The module reads the address of the user when it sends each fax. Thus, a change of the address applies immediately. |
| Attach | Empty (the value of the settings), `pdf`, `tiff` or `both`. |

You cannot delete a recipient if a route or a different object still
refers to it.

The PDF file has one page for each page of the fax, on the paper size of
the settings. The module makes the PDF file itself (`lib/FaxPdf.php`).
It does not decode the fax: the coded rows (MH, MR or MMR) go into the PDF
file without changes, with the `CCITTFaxDecode` filter. Colour and grey
pages (8 bits, uncompressed) go in with the `FlateDecode` filter. If the
module cannot make a PDF file from the TIFF file, it attaches the TIFF
file. The text of the message then gives the reason.

The module gives the message to the mail system of the machine with the
`mail()` function of PHP (on FreePBX, Postfix or Sendmail). Thus, the
mail system of the machine must send mail.

### Faxes

The **Faxes** tab is the fax log. It shows each received and each sent
fax, the newest fax first. For a sent fax, **To** is the fax number:

| column | value |
|---|---|
| Date | The time when the call ended. |
| From | The caller ID name and number. |
| Fax number | The fax number of the other machine (its TSI). |
| To | The fax recipient, if the fax came to a recipient. |
| Pages, Status, Rate | `DMODEMFAXPAGES`, `DMODEMFAXSTATUS` and `DMODEMFAXRATE`. |
| Email | **Sent** or **Not sent**, for a fax recipient, or **Blocked**. Put the mouse on it to see the message of the mail system, or why the fax is blocked. |

For each fax:

- **View PDF** opens the fax as a PDF file (the PDF paper size of the
  settings).
- **TIFF** downloads the fax file.
- **Block** (a received fax with a caller ID number) adds the caller ID
  number to **Block faxes from**.
- **Delete** deletes the entry and the fax file.

The log gets the faxes of the fax destination, of the fax recipients and
of the profiles with **Fax** on. A call that does not receive a fax
(for example a data call to a profile) does not go into the log.

The module deletes the faxes that are older than **Keep faxes** each
time that a fax arrives. The log keeps 5000 faxes at most. When there
are more, the module deletes the oldest faxes.

The files go to the browser only through the Ajax handler of FreePBX
(`ajax.php?module=dmodem&command=faxpdf|faxtiff&uniqueid=<id>`), and
only for an administrator who is signed in. The module sends only a
file of the fax directory that the log has.

The fax log is not in the backup or in `fwconsole dmodem export`. The
fax files are in the fax directory.

### Junk fax control

Three settings block the faxes from some caller IDs (`lib/FaxJunk.php`):

| setting | key | notes |
|---|---|---|
| Block faxes from | `fax_block` | The caller IDs to block. |
| Never block faxes from | `fax_allow` | The caller IDs that are never blocked. This list comes first. |
| Only the allow list | `fax_allow_only` | Block each caller ID that is not on **Never block faxes from**. Then that list must not be empty. |

Each list has one entry on each line, 500 entries at most:

- a number. The module compares only the digits, thus `+1 (555)
  010-0100` and `15550100100` are the same number;
- a prefix with `*` at the end, for example `1900*`: all numbers that
  start with these digits;
- `anonymous`: a call without a caller ID number (for example
  "anonymous" or "restricted").

The text after `#` on a line is a comment.

D-Modem receives a blocked fax, because the module examines the caller
ID when the call ends (`Dmodem::faxReceived()`). Then:

- the fax goes into the fax log with the status `BLOCKED`, and its
  file stays in the fax directory;
- the fax does not go by email to a fax recipient;
- the fax is not in the UCP Fax widget, and UCP shows no notification
  for it.

The control is the same for the two engines, because the hangup
handler gives the caller ID to `fwconsole dmodem fax2mail` for app_dmodem
and dmodem-ari.

### Send fax

The **Send fax** tab sends a fax through the dial-out context (your
outbound routes):

| field | notes |
|---|---|
| Fax number | Digits, `*`, `#` and `+`. |
| Documents | PDF, TIFF or Office files, 10 at most, 20 MB at most each. They go in this order. |
| Cover page | A first page with the company lines of the settings, the date, To, the fax number, From, the number of pages and the message. |
| To, From, Message | One line of 100 characters (To, From). The message has 3000 characters at most, and 36 lines show on the page. |
| Tries again | 0 to 3 (the settings give the default). |
| Email the result, Email address | When the result goes by email, and to whom. The message has the PDF file of the fax. |

When you click **Send**, the module makes the fax file
`<fax directory>/out-<job>.tif`:

1. A TIFF document goes into a PDF file (`lib/FaxPdf.php`). LibreOffice
   makes a PDF file of an Office document (`soffice --headless
   --convert-to pdf`, with its own profile in the temporary folder).
   The module finds an Office document by the extension of its name:
   `.doc`, `.docx`, `.odt`, `.rtf`, `.txt`, `.xls`, `.xlsx`, `.ods`,
   `.ppt`, `.pptx` and `.odp`.
2. Ghostscript makes the fax pages of the documents (`tiffg4`, 204 x
   196 dpi, 1728 dots a line, the PDF paper size of the settings). The
   module counts these pages for the cover page.
3. Ghostscript makes the fax file of the cover page (`lib/FaxCover.php`,
   our own PDF code) and the documents.

Then the job goes into the queue, and the first try starts
immediately. The module runs Ghostscript (with `-dSAFER`) and
LibreOffice without a shell.

A try is a call: the manager's `Originate` action dials
`Local/<number>@<dial-out context>`. When the far end answers, the call
goes to `app-dmodem-faxout`, which sends the fax file. If the far end
does not answer, Asterisk runs the `failed` extension of that context.
Each of the two gives the result to `fwconsole dmodem faxsent`.

If the try fails and the job has tries left, the next try is after the
minutes of the settings. FreePBX runs `fwconsole dmodem faxqueue` each
minute (a FreePBX job, `fwconsole job`, that the module adds when it is
installed), which starts each try at its time. A try without a result
after the ring time and one hour is a failure.

When the fax is sent, or the last try fails, the fax goes into the
[fax log](#faxes) (**Sent**), the job leaves the queue, and the result
goes by email if the job says so.

Below the form, the page shows the faxes that wait, with their state,
tries and last result. **Cancel** removes a fax that is not on a call.
It goes into the log as `CANCELLED`.

### Email to fax

A message to `<number>@<fax domain>` becomes a fax to that number
(`lib/Mail2Fax.php`, and `lib/MailParse.php`: our own reader of MIME
messages).

| setting | key | notes |
|---|---|---|
| Email to fax | `mail2fax` | Off by default. |
| Spool folder | `mail2fax_spool` | `/var/spool/asterisk/fax/mail2fax`. The mail system writes the messages here. |
| Fax domain | `mail2fax_domain` | Only the addresses in this domain give numbers. Empty: all domains. |
| Senders | `mail2fax_senders` | One address or `@domain` on each line, 500 at most. Email to fax needs one. |
| PIN | `mail2fax_pin` | 4 to 32 letters and digits. If set, the subject must have the PIN as a word. Empty: no PIN. |
| Cover page | `mail2fax_cover` | On by default: a cover page with the sender (From) and the text of the message. |

To set up the mail system (Postfix on FreePBX):

1. Make the spool folder. The mail system writes to it, and Asterisk
   reads and deletes the messages:

   ```sh
   install -d -m 1733 -o asterisk -g asterisk /var/spool/asterisk/fax/mail2fax
   ```

2. Add an alias with a pipe to `/etc/aliases`, then run `newaliases`:

   ```
   fax: "|/usr/bin/php /var/www/html/admin/modules/dmodem/bin/mail2fax-spool.php /var/spool/asterisk/fax/mail2fax"
   ```

3. Send all addresses of the fax domain to the alias. For example, in
   `/etc/postfix/main.cf`, add the domain to `mydestination` and set
   `virtual_alias_maps = regexp:/etc/postfix/fax_virtual`, with this
   line in `/etc/postfix/fax_virtual`:

   ```
   /^[0-9+*#().-]+@fax\.example\.com$/ fax
   ```

   Postfix keeps the original address in `X-Original-To`. Then run
   `postfix reload`.

`bin/mail2fax-spool.php` uses no code of FreePBX or of the module,
because it runs as the user of the mail system. It writes each message
to the spool as `<time>-<random>.eml` (30 MB at most), with a temporary
name first. On an error it exits with 75, and the mail system tries
again. It does nothing when it is not run on the command line.

Each minute, `fwconsole dmodem faxqueue` reads 20 messages of the spool
at most (the oldest first), deletes each file, and then starts the
tries. For each message:

1. A message without a sender on **Senders**, without the PIN, or from
   an automatic sender (`Auto-Submitted`, `Precedence: bulk`, `junk` or
   `list`, `List-Id`) is dropped without an answer.
2. The fax numbers come from `X-Original-To` and `Delivered-To`, or, if
   these have no number, from `To` and `Cc`: 10 numbers at most. The
   number is the part before `@` (or after its last `+`, as in
   `fax+5550100@...`), without `-`, `.`, `(` and `)`.
3. The documents are the attached PDF, TIFF and Office files. The
   module skips other files (for example, an image of a signature).
4. One job goes into the queue for each number, with the cover page.
   If there is no PIN, the subject is the first line of the message on
   the cover page. The result goes to the sender, as **Email the result**
   says.

When a known sender's fax cannot be sent (no number, no document, a
document that the module cannot read), the sender gets an answer with
`Auto-Submitted: auto-replied`.

A sender address is easy to forge. Set a PIN, or let the mail system
take mail for the fax domain only from your own network.

`fwconsole dmodem mail2fax <file>` (or `-` for standard input) puts one
message into the queue immediately, for example from procmail or
fetchmail as the asterisk user.

Email to fax is the same for the two engines, because it uses the queue
of [Send fax](#send-fax).

### Send API

Programs send faxes through the Ajax handler of FreePBX
(`ajax.php?module=dmodem`). The answers are JSON.

| command | method | parameters | answer |
|---|---|---|---|
| `sendfax` | POST, `multipart/form-data` | The fields of the [Send fax](#send-fax) form: `number`, `docs[]` (the files), `cover`, `to`, `from`, `message`, `retries`, `notify`, `email`. | `status`, `message`, `job` (the job ID), `errors` (field: message). |
| `faxstatus` | GET or POST | `job` | `status`, `job`, `state`, `number`, `pages`, `attempts`. In the queue (`queued`, `sending`, `retry`): also `next` and `last`. Done (`done`): also `result` (`SUCCESS`, `FAILED`, `NOANSWER`, `CANCELLED` and others), `uniqueid` and `time`. |

An administrator who is signed in can use the commands. A program must
send the token:

1. In the settings, set **Send API** on, and set a **Send API token**
   (32 to 128 letters, digits, `_` and `-`, for example `openssl rand
   -hex 32`).
2. Send the token in the `X-Dmodem-Token` header, or as
   `Authorization: Bearer <token>`.

With a good token, the module tells FreePBX not to examine the session
and the referrer (`authenticate` and `allowremote`). The token opens
only `sendfax` and `faxstatus`. The module compares the token with
`hash_equals()`. Use HTTPS, because the token is a password.

For example:

```sh
curl -H "X-Dmodem-Token: $TOKEN" \
     -F number=5550100 -F cover=yes -F message=Hello \
     -F 'docs[]=@invoice.pdf' \
     'https://pbx.example.com/admin/ajax.php?module=dmodem&command=sendfax'
curl -H "X-Dmodem-Token: $TOKEN" \
     'https://pbx.example.com/admin/ajax.php?module=dmodem&command=faxstatus&job=j1800000000a1b2c3'
```

A fax from the API has no User Manager user. Thus, it is not in the
UCP Fax widget of a user.

### Dial out

A call to a remote modem (a BBS, an ISP) with a profile:

1. The module dials the number (digits, `*`, `#` and `+`, 40 at most) in the
   dial-out context. That is `from-internal` by default. Thus, your outbound
   routes select the trunk, as for the call of an extension.
2. The call uses the caller ID of the settings (or the given caller ID) and the ring
   time.
3. When the far end answers, D-Modem is the calling modem:
   `DModem(<profile>,o)`, or `Stasis(<app>,<profile>,o)` with dmodem-ari.
   Then the server or program of the profile communicates with the remote end.

The module asks the manager of Asterisk (AMI) to originate the call, with the
channel `Local/<number>@<context>` and the channel variable
`DMODEM_DIALOUT=<profile>`.

### Inbound routes: fax detection

The module adds a **D-Modem fax** tab to the page of each inbound route
(**Connectivity > Inbound Routes**). This is the "Detect Faxes" function of
the Fax module of FreePBX. It operates with the two engines.

| field | default | notes |
|---|---|---|
| Detect faxes | off | Listen for a fax machine on the calls of the route. |
| Listen for | CNG and T.38 | The calling tone (CNG) and the T.38 request, the CNG only, or T.38 only. |
| Time | 4 s | 1 to 30 s. |
| Ring while listening | on | The caller hears the ring tone while D-Modem listens. |
| Fax destination | none | Necessary. For example a fax recipient, or "D-Modem: receive a fax". |

When the tab is on, the route answers the call and listens. A fax goes
to the fax destination. Other calls go to the destination of the route.
Use this tab or the Fax tab of the Fax module on a route, not the two
together.

With dmodem-ari, D-Modem hears a T.38 request only with the SIP side of
dmodem-ari (refer to [ari-details.md](ari-details.md)).

The module keeps the fax detection of a route under the DID and the
caller ID number of the route. If you change the numbers of the route,
the fax detection moves with the route. If you delete the route, the
module deletes its fax detection. If a value on the tab is not correct
(for example, no fax destination), the module keeps the previous fax
detection of the route.

The fax destination is a use of that destination: the destination
pages of FreePBX show the route (`dmodem_check_destinations()`).

### Line check

The check also shows what the fax machine of the engine can do. For
app_dmodem, it is the output of `dmodem fax show capabilities`. For
dmodem-ari, it is fixed text, with T.38 if the settings enable the SIP
side.

The line check reads each PJSIP endpoint (up to 200), as Asterisk has it
now, through the manager. It reports the items that are important for a modem call:

- no G.711 (a warning), or codecs other than G.711 (a warning).
  The check shows CLEARMODE (of `codec_clearmode`) separately, for ISDN data calls;
- the two laws permitted (a warning: a call can be converted between them,
  and then V.90 is not possible);
- DTMF detected in the audio (`inband`, or a mode that starts with `auto`:
  a warning);
- fax detection enabled (a warning: the 2100 Hz answer tone of the modem sounds
  like the tone of a fax machine);
- direct media enabled, and T.38 enabled (notes).

It also shows if app_dmodem is loaded (`module show like
app_dmodem`), or if dmodem-ari is connected (its Stasis app in `ari
show apps`).

### Generated files

This part shows the files that Apply Config writes.

## UCP: the Fax widget

UCP (the User Control Panel of FreePBX) shows a **Fax** widget to each
user that has fax on. The `ucp/` folder of the module has the widget.
When UCP is installed, FreePBX links this folder into UCP.

### The rights in User Manager

The **D-Modem** part of the UCP tab of a user or a group in User Manager
has these settings:

| setting | key | notes |
|---|---|---|
| Fax | `fax_enable` | Yes or no. A user can also inherit the value of its groups. |
| Send faxes | `fax_send` | Yes or no, or inherit. Without it, the user can only see faxes. |
| Fax recipients | `fax_recipients` | The fax recipients that the user sees. |

The module keeps these settings in UCP (`setSettingByID()` and
`setSettingByGID()`, module `Dmodem`). It reads them with
`getCombinedSettingByID()`, which merges the user and its groups.

A user also sees the faxes of each fax recipient whose User Manager
user is that user.

### The widget

The widget has five tabs:

| tab | content |
|---|---|
| Inbox | The received faxes of the user's fax recipients (not the blocked faxes). |
| Sent | The faxes that the user sent with the result `SUCCESS`. |
| Failed | The faxes that the user sent with a different result. |
| Outgoing | The user's faxes in the queue. **Cancel** removes a fax that is not on a call. |
| Send | The form of the [Send fax](#send-fax) tab: the fax number, the documents, the cover page. |

Each fax in a tab has these buttons: **View** (the PDF file),
**Download** (the TIFF file), **Delete**, and, with the send right,
**Forward** (send the fax file to a different number).

The module keeps the User Manager user ID in each fax that the widget
sends (`user` in the job and in the fax log). A user sees only the
faxes that it sent.

When the widget is open, UCP polls the module. When a new fax comes to
the inbox, the browser shows a notification (if the user allowed
notifications).

### The Ajax commands

| command | right | does |
|---|---|---|
| `faxlist` | fax | Gives the widget HTML again. |
| `faxpdf`, `faxtiff` | fax | Gives the file of a fax of the user. |
| `faxdelete` | fax | Deletes a fax of the user. |
| `faxsend` | send | Puts a fax into the queue (`Dmodem::queueFax()`). |
| `faxforward` | send | Puts a fax of the user into the queue to a new number. |
| `faxcancel` | send | Cancels a fax of the user in the queue. |

After `faxsend` and `faxforward`, the module starts the queue
immediately. A command for a fax of a different user fails with "No
such fax".

## What Apply Config writes

Apply Config writes `/etc/asterisk/dmodem.conf` (with the header of FreePBX
first). Do not edit that file manually, because the next reload writes
it again.
Apply Config also writes the `app-dmodem` context into
`extensions_additional.conf`:

```
[app-dmodem]
exten => bbs,1,NoOp(D-Modem profile bbs)
exten => bbs,n,Set(CHANNEL(hangup_handler_push)=app-dmodem-fax2mail,s,1())
exten => bbs,n,DModem(bbs)
exten => bbs,n,NoOp(D-Modem bbs: ${DMODEMSTATUS} ${DMODEMRATE})
exten => bbs,n,Hangup()
exten => receivefax,1,NoOp(D-Modem fax)
exten => receivefax,n,Set(CHANNEL(hangup_handler_push)=app-dmodem-fax2mail,s,1())
exten => receivefax,n,DModemReceiveFax(/var/spool/asterisk/fax/${STRFTIME(${EPOCH},,%Y%m%d-%H%M%S)}-${FILTER(0-9A-Za-z+,${CALLERID(num)})}-${UNIQUEID}.tif)
...
exten => fax-sales,1,NoOp(D-Modem fax to sales)
exten => fax-sales,n,Set(CHANNEL(hangup_handler_push)=app-dmodem-fax2mail,s,1(sales))
exten => fax-sales,n,DModemReceiveFax(<the same file name>)
...

[app-dmodem-faxout]
exten => s,1,NoOp(D-Modem fax job ${FILTER(a-z0-9,${DMODEMFAXJOB})})
exten => s,n,Set(CHANNEL(hangup_handler_push)=app-dmodem-faxsent,s,1(${FILTER(a-z0-9,${DMODEMFAXJOB})}))
exten => s,n,DModemSendFax(/var/spool/asterisk/fax/out-${FILTER(a-z0-9,${DMODEMFAXJOB})}.tif)
...
exten => failed,1,System(/usr/sbin/fwconsole dmodem faxsent '${FILTER(a-z0-9,${DMODEMFAXJOB})}' '${FILTER(0-9.,${UNIQUEID})}' 'NOANSWER' ...)

[app-dmodem-faxsent]
exten => s,1,System(/usr/sbin/fwconsole dmodem faxsent '${FILTER(a-z0-9,${ARG1})}' '${FILTER(0-9.,${UNIQUEID})}' ...)
exten => s,n,Return()

[app-dmodem-fax2mail]
exten => s,1,ExecIf($["${DMODEMFAXSTATUS}" = ""]?Return())
exten => s,n,System(/usr/sbin/fwconsole dmodem fax2mail '${FILTER(a-z0-9_,${ARG1})}' '${FILTER(0-9.,${UNIQUEID})}' ...)
exten => s,n,Return()
```

The `receivefax` extension, each profile with **Fax** on, and each fax
recipient push the hangup handler. Each fax recipient has an extension
`fax-<id>`. It gives its id to the hangup handler, then receives the fax
as `receivefax` does. The other extensions give no id.

When the call ends, the hangup handler does nothing if the call did not
receive a fax (`DMODEMFAXSTATUS` is empty). Else it runs
`fwconsole dmodem fax2mail` with the id of the recipient (empty for no
recipient) and the results of the call: the unique ID,
`DMODEMFAXSTATUS`, `DMODEMFAXPAGES`, the caller ID number,
`DMODEMFAXREMOTE`, the caller ID name and `DMODEMFAXRATE`. The command
puts the fax into the [fax log](#faxes). For a recipient, it then sends
the fax by email. `FILTER()` keeps only the characters that each value can have,
and each value is in `'...'`. Thus, no data of the call gets to the shell
as a command. The path of `fwconsole` is from the `AMPSBIN` setting of
FreePBX (`/usr/sbin` if that setting is not a plain path). The module
always writes the `app-dmodem-fax2mail` context.

`fwconsole dmodem fax2mail` finds the fax file of the call by its unique
ID (`*-<uniqueid>.tif` in the fax directory). If the call has the status
`FAILED` and no pages, it sends no message.

The two engines set the `DMODEMFAX*` variables on the channel. Thus, the
hangup handler is the same for the two engines.

For an inbound route with fax detection, the dialplan hook of the module
(priority 500, after Core at 480) adds two steps to the extension of the
route in `ext-did-0001` or `ext-did-0002`. The steps go immediately
before the `did-cid-hook` label of Core:

```
exten => 5551002,n,DModemFaxDetect(4,xr)
exten => 5551002,n,GotoIf($["${DMODEMFAXDETECTED}" = "CNG" | "${DMODEMFAXDETECTED}" = "T38"]?app-dmodem,fax-sales,1)
exten => 5551002,n(did-cid-hook),NoOp(CallerID Entry Point)
```

With dmodem-ari, the first step is
`Stasis(dmodem,faxdetect,4,xr)`. The option `x` tells the engine not to
go to a `fax` extension, because each route has its own fax
destination. The option `r` is the ring tone, and `c` or `t` is the CNG
only or T.38 only. If the extension of the route is not in those
contexts, the module adds nothing.

With dmodem-ari as the engine, `DModem(bbs)` is `Stasis(dmodem,bbs)`, and
the fax step is `Stasis(dmodem,receivefax,<file>)`. The name of the fax file is
the same as the name that `DModem()` gives to the faxes that it receives automatically. One
difference: if the caller ID is empty, that part of the name is empty (`DModem()` writes `unknown`).

app_dmodem reads `dmodem.conf` on each call. Thus, a change applies from the
next call. dmodem-ari reads it when it starts. Thus, restart dmodem-ari after Apply
Config.

When you uninstall the module, it deletes its stored settings and profiles.
It does not delete `dmodem.conf`. The next reload removes it.

## fwconsole dmodem

```
fwconsole dmodem dial <profile> <number> [--callerid='"BBS" <5550100>']
fwconsole dmodem profiles
fwconsole dmodem export [file]          # the settings and profiles, as JSON
fwconsole dmodem import <file> [--keep] # back in; --keep keeps the profiles
                                        # that are not in the file
fwconsole dmodem fax2mail <recipient|""> <uniqueid> [status] [pages] [cid] [remote] [cidname] [rate]
                                        # the fax of a call into the log,
                                        # and by email to a recipient
fwconsole dmodem faxes                  # the fax log
fwconsole dmodem sendfax <number> [file...] [--cover] [--to=...] [--from=...]
          [--message=...] [--retries=n] [--notify=never|always|success|failure]
          [--email=...]                 # a fax into the queue
fwconsole dmodem faxqueue               # the email to fax spool, then the
                                        # tries whose time has come
fwconsole dmodem mail2fax <file|->      # email to fax: one message
fwconsole dmodem faxjobs                # the queue
fwconsole dmodem faxcancel <job>        # a fax out of the queue
fwconsole dmodem faxsent <job> <uniqueid> <status> [pages] [rate] [error] [remote]
                                        # the result of a try
```

- `dial` is the Dial out page for scripts (cron, the mailer of a BBS).
- `export` without a file shows the JSON. An export holds the ARI
  password. Thus, only its owner can read a file that it writes (mode
  0600).
- `fax2mail` is the command of the hangup handler (refer to
  [What Apply Config writes](#what-apply-config-writes)). You can also
  use it to send a fax again. An empty recipient (`""`) puts the fax
  into the log only.
- `faxes` shows the fax log: the date, the unique ID, the status, the
  pages, the caller ID and the recipient of each fax.
- `sendfax` is the Send fax tab for scripts. Without `--retries`,
  `--notify` and `--email`, the settings give the values. It starts
  the first try immediately.
- `mail2fax` is [email to fax](#email-to-fax) for one message.
- `faxqueue` is the command of the FreePBX job. `faxsent` is the command
  of `app-dmodem-faxout` (refer to
  [What Apply Config writes](#what-apply-config-writes)).
- `import` checks the settings, each profile and each fax recipient as
  the forms do (refer to
  [Backup and restore](#backup-and-restore)). After an import, run
  `fwconsole reload` to write the files.

## Backup and restore

The module is a part of the Backup & Restore of FreePBX (`Backup.php`,
`Restore.php`). A backup that includes D-Modem keeps its settings,
profiles, fax recipients and the fax detection of the inbound routes:
the same data as `fwconsole dmodem export`.

A restore (as `fwconsole dmodem import` without `--keep`) checks the
data as the forms do:

- The restore checks the settings together. If one setting fails, the
  restore does not restore a setting. The log of the restore gives the
  names of the settings that failed.
- The restore checks each profile separately. If a profile fails (for
  example a profile with the name `general`), the restore does not
  restore it, and gives its name in the log. The restore restores the
  other profiles.
- The restore deletes the profiles that are not in the backup.
- The restore checks the fax recipients in the same way as the profiles.
  The log gives a recipient that failed as `fax-<id>`. If the backup has
  no fax recipients (a backup of version 16.0.2), the restore keeps the
  recipients that there are. The same applies to the fax detection of
  the inbound routes (`did-<DID>/<caller ID>` in the log).

The reload after the restore makes `dmodem.conf` and the dialplan again.
Received faxes are files in the fax directory, outside the
data of the module. Thus, a backup of the module does not include them.

## The path of the call

V.90 must have the G.711 of the call without changes from the network to the modem
([asterisk-details.md](asterisk-details.md#the-channel)). In FreePBX:

- In Settings > Asterisk SIP Settings, and on the trunk and the modem
  extension, permit only one law: the law that your provider uses (ulaw in
  North America and Japan, alaw in other countries). Also, your provider must not
  transcode.
- On those endpoints: DTMF RFC 4733, Fax Detect disabled, and T.38 disabled where
  no fax must use it.
- On the inbound route of the modem: Detect Faxes no, no call recording, and
  no music on hold or announcement before the answer.

The line check shows the items that are still incorrect.

## The module's code

- `module.xml`: the name, version (16.0.9), menu item, console
  command and FreePBX versions of the module.
- `Dmodem.class.php`: the BMO (the module object of FreePBX) class. It has the
  forms (`doConfigPageInit()`), the pages, `genConfig()` and
  `writeConfig()` for `dmodem.conf`, the dialplan hook
  (`doDialplanHook()`), the destinations, the dial-out (`dialOut()`), the
  export and import (`exportData()`, `importData()`) and the line check
  (`check()`), the fax recipients and `faxToMail()`, and the fax
  detection of the inbound routes (`myConfigPageInits()`,
  `didFaxTab()`), and the fax log (`faxReceived()`, `getFaxes()`,
  `deleteFax()`, `purgeFaxes()`, and `ajaxRequest()` and
  `ajaxCustomHandler()` for the files), and the send API
  (`ajaxHandler()`, `faxStatus()`). The settings, profiles, fax
  recipients, the fax detection of the routes and the fax log are in
  the kvstore of the module.
- `lib/Config.php`: the defaults and checks of the settings and profiles, and
  `dmodem.conf` made from them. A `;` in a value is written `\;`, as
  Asterisk and dmodem-ari read it. No line breaks go through.
- `lib/Dialplan.php`: the `app-dmodem` context, and the
  `app-dmodem-fax2mail` context of the hangup handler.
- `lib/FaxPdf.php`: a TIFF file of a fax as a PDF file.
- `lib/FaxLog.php`: the entry of the fax log, its checks, and the
  entries to delete.
- `lib/FaxSend.php`: a fax to send: the job and its checks, the
  Ghostscript and LibreOffice commands, the fax file, the `Originate` action of a try, and
  the state after a try.
- `lib/FaxJunk.php`: the junk fax control: the block list and the
  allow list of caller IDs, and their checks.
- `lib/MailParse.php`: our own reader of an email message (headers,
  encoded words, RFC 2231 names, addresses, multipart, base64 and
  quoted-printable, the text and the files).
- `lib/Mail2Fax.php`: email to fax: the numbers of the addresses, the
  senders, the PIN, and the job of each number.
- `bin/mail2fax-spool.php`: email to fax: a message from the mail
  system into the spool folder.
- `lib/FaxCover.php`: the cover page as a PDF file (Helvetica, no font
  files).
- `lib/FaxMail.php`: the message of a fax (MIME, with the subject and
  text from the templates) to the mail system.
- `lib/Apps.php`: the dialplan step class (`ext_dmodem_app`) for
  `DModem()`, `Stasis()` and `DModemReceiveFax()`. The
  extensions class of FreePBX has no classes for these.
- `lib/LineCheck.php`: reads `pjsip show endpoints`, `pjsip show endpoint
  <name>`, `module show` and `ari show apps`, and finds the meaning of the
  settings of an endpoint for a modem.
- `lib/Dialout.php`: the Originate action of a call out, and the checks of its
  number and caller ID (no line breaks in the lines to the manager).
- `lib/Cli.php`: the subcommands of `fwconsole dmodem`
  (`Console/Dmodem.class.php` gives them to it). Thus, they operate
  without Symfony.
- `Backup.php`, `Restore.php`: Backup & Restore (through
  `Dmodem::exportData()` and `importData()`).
- `functions.inc.php`: the destination hooks of FreePBX, and
  `dmodem_check_destinations()`.
- `module.xml` `<hooks>`: the tab on the inbound route page (Core's
  `hookTabs`).
- `page.dmodem.php`, `views/`, `assets/js/dmodem.js`: the pages.
- `ucp/Dmodem.class.php`, `ucp/views/widget.php`,
  `ucp/assets/js/global.js`: the UCP Fax widget (`UCP\Modules\Dmodem`).
  It calls `Dmodem::ucpFaxAccess()`, `ucpFaxes()` and `ucpFaxJobs()`.
- `module.xml` `<hooks>` for `Ucp`: the D-Modem part of the UCP tab in
  User Manager (`constructModuleConfigPages` to `Dmodem::ucpConfigPage()`,
  `views/ucp_user.php`), and its save (`addUser`, `updateUser`,
  `addGroup`, `updateGroup`).

## Tests

### unit.php

`frontend/freepbx/test/unit.php` (ctest `freepbx_unit`, label `quick`,
registered when PHP is installed) tests the module without FreePBX. It runs on stand-ins
for the items that the module uses (`fpbx_env.php`):

- the kvstore, with the rules of DB_Helper (a stored false deletes the
  key, and values come back as strings);
- `extensions::add()` and its output, as the
  `extensions.class.php` of FreePBX has them;
- the manager, with canned CLI output;
- the destination usage.

It checks:

- the forms, with good and bad values;
- `dmodem.conf`, read back as dmodem-ari reads it;
- the dialplan for both engines;
- the destinations;
- the PDF of TIFF files that the test makes (MH, MR, MMR, uncompressed,
  colour, fill order 2), the TIFF files that it refuses, and, if Poppler
  is installed, the PDF as `pdftoppm` shows it;
- the message (its headers, its text and its attachments) to a stand-in
  mail system, and the fax recipients (forms, User Manager, the
  destination, the hangup handler, the pages);
- the fax log: `fwconsole dmodem fax2mail` with and without a
  recipient, `fwconsole dmodem faxes`, the PDF and TIFF through the
  Ajax handler, the Faxes page, the delete action, **Keep faxes** and
  the limit of 5000 faxes;
- the junk fax control: the lists and their checks, numbers, prefixes
  and `anonymous`, the allow list first, **Only the allow list**, a
  blocked fax (logged as `BLOCKED`, not mailed, not in UCP) and the
  **Block** button;
- the faxes to send: the cover page (and, if Poppler is installed, the
  page as `pdftoppm` shows it), the checks of a job, the Ghostscript
  and LibreOffice commands (with stand-ins, and with the real programs
  if they are installed), the send API (the token, `sendfax`,
  `faxstatus`), email to fax (the message reader, the numbers, the
  senders, the PIN, automatic messages, the refusal, the spool and
  `bin/mail2fax-spool.php`, `fwconsole dmodem mail2fax`), the Ghostscript
  command (with a stand-in, and with the real `gs` if it is installed),
  the queue (the `Originate`, a failed try and the next try, the result
  in the log and by email, no answer, a refused call, a try without a
  result, cancel), `fwconsole dmodem sendfax`, the page, the contexts
  for the two engines and the FreePBX job;
- the UCP Fax widget, on stand-ins of UCP and User Manager
  (`ucp_env.php`): the rights of users and groups (and inherit), the
  User Manager page and its save, the widget list, the folders, the
  poll, the files, delete, send, forward and cancel, and the faxes of
  a different user;
- the fax detection of the inbound routes: the form of the route
  (added, changed, renamed, deleted, bad values), the steps in a route
  as Core writes it for the two engines, the tab and the destination
  usage;
- the line check, on the CLI output of Asterisk 20 (`test/fixtures/`);
- the dial-out (its Originate, and refused numbers and caller IDs);
- `fwconsole dmodem`;
- an export imported elsewhere;
- the Backup and Restore classes of the module, on stand-ins of the base classes of the backup
  module (`backup_env.php`);
- the pages.

`FREEPBX_FRAMEWORK=<a checkout of FreePBX's framework>` runs the dialplan
through the `extensions.class.php` of FreePBX instead of the stand-in.

### e2e.sh

`frontend/freepbx/test/e2e.sh` runs the output of the module in a private Asterisk
(`ARI=1`: with dmodem-ari as the engine):

1. `gen.php` makes `dmodem.conf` and the `app-dmodem` context in the same
   method as the page (with its forms posted).
2. An inbound route sends 5551000 to `app-dmodem,bbs,1`. dmodemd calls it
   over SIP and uses the BBS. The test makes sure that the call went through
   `app-dmodem,bbs,1`. (Two D-Modems on a clean G.711 path connect with
   V.91, at 62666/62666 bit/s.)
3. A call out: `dial.php` (as `fwconsole dmodem dial`) tells the manager to
   originate it through the dial-out context (here `modem-out`, the
   stand-in of the test for `from-internal`) to a dmodemd that listens. `DModem(term,o)`
   calls. The test makes sure that the call used `Local/5551000@modem-out`.
4. The line check runs on the live endpoints through the manager
   (`linecheck.php`, `ami.php`). The endpoint of the test permits the two laws.
   The check must show this as a warning.
5. Fax to email: `DModemSendFax` (`ARI=1`: dmodem-ari) sends a fax of
   two pages to `app-dmodem,fax-sales,1`. When the call ends, the hangup
   handler runs `fwconsole dmodem fax2mail`. Here, `fwconsole` is a
   stand-in that runs `fax2mail.php`, which keeps the message in a file.
   The message must go to the recipient, and have the text, a PDF file of
   two pages and the TIFF file.
6. Fax detection on an inbound route: `gen.php` adds a route 5551002 in
   `ext-did-0001` (a part of the route that Core writes) with fax detection
   to the recipient. A fax machine calls the route. The fax must go to
   `fax-sales` and come as a second message. Then a call without a fax
   must go to the destination of the route (`app-dmodem,bbs,1`).
7. The fax log: a fax to `app-dmodem,receivefax,1` (no recipient) must
   run `fwconsole dmodem fax2mail` with an empty recipient, and go into
   the log. The test then makes sure that each of the three faxes gave
   its rate, and that the data calls did not run the command.
8. Send a fax: `sendfax.php` (as `fwconsole dmodem sendfax`) queues the
   test document with a cover page to 5559999 in the dial-out context,
   where `DModemReceiveFax` takes it. Ghostscript (`gs`) must be
   installed. The far machine must get the pages of the document and the
   cover page, and the fax must go into the log. Then a fax to 5559998,
   which does not answer, must give `NOANSWER` through the `failed`
   extension. `dmodem_test_store()` keeps the queue and the log between
   the scripts.
9. Email to fax: `bin/mail2fax-spool.php` puts a message to
   5559999@fax.example.com (with the test document) into the spool.
   `fwconsole dmodem faxqueue` must queue it, and the far machine must
   get the pages of the document and the cover page.
