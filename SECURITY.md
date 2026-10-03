# Security

## Reporting a vulnerability
Please **don't** open a public issue for a security problem. Use GitHub's private vulnerability reporting
(Security tab → "Report a vulnerability") at <https://github.com/tayloraaron078-tech/ForcaSlicer/security>.
This is a one-person hobby project: expect a reply within a week or two, and please allow time for a fix before
publishing details.

## Forca AI — how it is protected
Forca AI lets an AI assistant operate the slicer. It is designed so that a mistake by the AI (or someone trying to
misuse it) can't cost you files or start a print.

- **Off by default.** Nothing listens until you tick "Let an AI connect to Forca" in the Forca AI window.
- **This computer only.** The server listens on `127.0.0.1` (port 13630) — not on your network.
- **A secret key.** Every request needs the key shown in the Forca AI window (stored in Forca's settings folder,
  `forca/ai/key.txt`). "Make a new key" invalidates the old one. Requests from web pages (a browser `Origin` header
  that isn't local) are refused, so a website can't drive Forca.
- **Your files are never overwritten.** Every file the AI saves gets a new "(Claude <date>)" name. It may only update
  files it created itself and you haven't changed since (tracked in `forca/ai/ledger.json`). It can't delete files and
  never edits your presets or discards your unsaved changes.
- **No print without you.** The AI can only request a print; Forca shows you an approval card, and only your click
  opens the normal print dialog, where you still choose the printer and press Send. An approval works once, for that
  exact sliced file. There is no command for the AI to approve, and a chat message is not an approval.
- **Control levels.** Everything above describes the default **Guarded** level. In the Forca AI window you can choose
  **Advanced** (with a warning to confirm); no AI command can change the level. At Advanced the AI may overwrite your
  files and edit your user presets, but Forca first copies each file into `Documents\Forca AI\Backups` (never
  deleted by Forca), and a print request opens the print dialog directly instead of showing a card -- you still press
  Send. At every level the AI can't delete files, discard your unsaved changes, change system presets, or heat, move
  or reconfigure a printer. The top bar shows "(Advanced)" while it is on.
- **Print grants (Advanced only).** You can let the AI start prints itself: a grant names the printers, the number of
  prints, how long it lasts and the longest print allowed. Forca sends a grant print only to a shared printer that is
  selected in the Device tab, live and idle, and only when its bed is clear -- you clicked "Bed is clear" and the
  printer has run nothing since (one clearing, one print), or, if you also ticked it, the AI judged a fresh camera
  picture (kept in `Documents\Forca AI\Bed checks`). The AI must name the printer slot for every filament (an AMS
  slot or the external spool); Forca checks that slot holds the right material and that the print dialog uses exactly
  those slots, before the countdown and again before Send. It goes through the normal print dialog: Forca waits until the
  dialog has nothing to ask you, shows a 10-second countdown with Cancel, checks everything again and presses Send;
  where the dialog would ask you to confirm a warning, the AI's send stops instead. A grant ends when used up, at its
  time limit, on Revoke, on choosing Guarded, or when Forca restarts. The top bar shows how many prints are left.
  A grant can also name the files to print: then Forca sends only plates whose every object was imported from those
  files (by each object's source file), never more copies than the grant still owes.
- **Printers are opt-in.** The AI sees only printers you tick. For those it can read status and take a camera
  snapshot. Pausing a failing print needs a second, separate tick; it may resume only its own pause.
- **No secrets to the AI.** Printer access codes, passwords, API keys and printer network addresses are never
  returned to it.
- **Everything is logged** in the Forca AI window and in `forca/ai/activity.jsonl`, and every AI edit is one undo
  step (Ctrl+Z).

What it does not protect against: anything with access to your user account can read the key file, and an AI you
connect sees the models and settings you work on. Only connect assistants you trust.
