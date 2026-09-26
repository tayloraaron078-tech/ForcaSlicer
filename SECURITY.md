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
- **Printers are opt-in.** The AI sees only printers you tick. For those it can read status and take a camera
  snapshot. Pausing a failing print needs a second, separate tick; it may resume only its own pause.
- **No secrets to the AI.** Printer access codes, passwords, API keys and printer network addresses are never
  returned to it.
- **Everything is logged** in the Forca AI window and in `forca/ai/activity.jsonl`, and every AI edit is one undo
  step (Ctrl+Z).

What it does not protect against: anything with access to your user account can read the key file, and an AI you
connect sees the models and settings you work on. Only connect assistants you trust.
