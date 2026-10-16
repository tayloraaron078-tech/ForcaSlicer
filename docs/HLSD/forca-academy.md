# Forca Academy: High Level Design

## Why it exists

Slicer settings are learned by printing: a PETG part strings, a setting is changed, the next one doesn't. Slicers keep
none of that. Forca Academy is a print journal and knowledge base in a plain folder of Markdown and JSON, so the
history of what was printed, how, and how it went is kept on the user's disk, readable by the user, by Forca, and by
any AI the user chooses to point at it. Nothing is uploaded.

Forca writes facts; the user (or the user's AI, within Forca AI's write rules) writes judgments: outcomes, notes,
lessons. Forca only ever proposes a judgment.

## What the user sees

- Preferences > Forca Academy: "Keep a print journal" (off by default), camera pictures at the end of a print and
  during it, the folder (default `Documents\Forca Academy`), and "Show \"Before you print\" when sending".
- File > Print Journal: the recorded prints, newest first; per print its picture, details, outcome (Success /
  Partial / Failed), quick "what went wrong" tags, a note and photos. "Add a print" records the current plate for
  prints Forca didn't send. "Insights" groups the prints by printer, filament preset or process preset (prints,
  outcomes, most common problem, last failure); double-clicking a group opens the journal on its prints. Insights
  also lists possible patterns (the same problem tag on N of the last M prints with an outcome for one printer and
  filament preset; 3 of 10 by default, set there or in Preferences), each with "Show prints", "Write a lesson" (a
  draft for `lessons.md` the user edits and rates) and "Don't show again", and the experiments.
- Selecting two prints and pressing "Compare" shows them side by side: printer, presets, estimates, outcome and tags,
  then the settings that differed. "Save as experiment" writes `experiments/<date>_<name>.md` with both prints, what
  differed and a Result section for the user. The journal's "Playbooks" button opens the playbook for a problem tag.
- The send dialogs (Bambu's print dialog and the print-host upload dialogs) show "Before you print": how earlier prints
  with the same printer and filament preset went, the settings changed and not saved, and collapsed Details (layer
  height, sparse infill pattern and density, supports, brim, nozzle, and per filament its temperatures, bed
  temperature and max volumetric speed). It never blocks or delays a send.

## The folder

```
AGENTS.md, INDEX.md, lessons.md        starter files, written once; never overwritten (the user owns them)
printers/ materials/ experiments/
playbooks/<tag>.md                     a starter per journal tag, written once; never overwritten or recreated
prints/YYYY/YYYY-MM-DD_HHMM_<slug>/    record.json, settings.json, notes.md, thumbnail.png, photos/, camera/
forca/insights.json                    Forca's own state: dismissed patterns, which starters it already wrote
```

`record.json` is written by Forca when a print is sent: printer (Bambu device name, else physical printer, else
printer preset), process and filament presets with the tray each slot printed from and its use, the settings
changed and not saved, plate and objects, estimates, the project file, and later the outcome and how the printer
said the print ended. `notes.md` mirrors the outcome and the print end in Forca-owned blocks between
`<!-- forca:... -->` markers; text outside them, or a block whose markers the user removed, is left alone. Printer
pages carry a Forca-owned facts block the same way, and calibration results append to printer and material pages.
`settings.json` holds every setting the print used (process, printer and filament values from the full config,
preset bookkeeping keys left out), kept apart so `record.json` stays readable; prints recorded before it existed
have none.

`INDEX.md` gets a Forca-owned facts block too (print counts and success rates per printer and filament, open patterns,
the latest lessons), refreshed after every record, outcome and saved lesson. It is inserted under the title once
(remembered in `forca/insights.json`); after that it is only replaced between its markers, so a page whose block the
user removed stays as it is. The block doesn't make `INDEX.md` the user's page for the AI write rules, and an
`INDEX.md` the AI wrote stays the AI's after a refresh.

## How it works

`slic3r/GUI/ForcaAcademy.{hpp,cpp}` holds the folder logic (no GUI in the file side, unit-tested in
`tests/slic3rutils/test_forca_academy.cpp`) and the hooks; `ForcaPrintJournal.{hpp,cpp}` the journal and Insights
dialogs; `ForcaBeforeYouPrint.{hpp,cpp}` the send dialogs' block; `libslic3r/ForcaPreflight.{hpp,cpp}` the
config-only part of that block (tested in `tests/libslic3r/test_forca_preflight.cpp`); `ForcaAIAcademy.cpp` the
Forca AI tools.

- **The folder.** The default is `Documents/Forca Academy`. A folder picked in Preferences gets a `Forca Academy` folder
  inside it (`forca_academy_folder_in`), so Forca never fills a folder that holds other things, and a reset never moves
  one. A folder already named so, or one that already holds a journal at its top (picked before this rule), is used
  as it is.
- **Recording.** Only prints Forca sends get a record automatically: a Bambu send that succeeded
  (`Plater` on the print-sent event, not a reprint from the printer's storage), or a print-host upload that finished
  with "print" as its post-action. The plate is captured on the GUI thread at send time, so later edits don't leak in.
- **Following a print to its end.** A sent print is tracked in `<data dir>/forca/academy_tracking.json`, so it
  survives a restart. A 15-second timer reads the printer's state (Bambu: the Device tab's printer; Klipper and
  stock Flashforge printers: their status readers). Once a tracked print has been running and then finishes, fails
  or is replaced by another job, the end goes into the record, with an end photo when the printer has a camera Forca
  can reach. Entries never seen running drop after 6 hours, any entry after 7 days.
- **Before you print.** The history comes from the journal list: each listed print carries its printer, process,
  filaments used and outcome tags, and `forca_academy_history` counts the prints with the same printer and filament
  preset, with outcomes counted only when the user recorded one. At send time the printer is named as records name
  it (Bambu: `forca_ai_printer_label`, print hosts: the physical printer, else the preset). Changed settings compare
  each preset collection's edited preset with its saved one. The panel is rebuilt whenever Bambu's (reused) dialog
  shows or its plate or printer changes; the print-host dialogs build it once per send.
- **Compare.** `forca_academy_compare` lists the records' facts for both prints, then, when both have
  `settings.json`, every setting whose value differs; otherwise it falls back to the settings each print had changed
  from its presets (`record.json` "unsaved_changes"), since the presets themselves may have changed since.
- **Patterns and lessons.** `forca_academy_patterns` takes, per printer and filament preset, the newest M prints with an
  outcome and reports each tag found on at least N of them. A tag counts on any print with an outcome, a successful
  one included, and the pattern also says on how many of those the print was successful. Dismissing a pattern, or saving a lesson from it, adds its
  id (printer, filament, tag) to `forca/insights.json`. Forca drafts the lesson text; it is saved only when the user
  presses the button.
- **Forca AI.** With the Academy on, the AI can read the folder, list and search prints, see the Insights
  (`forca_academy_insights`: groups, open patterns, experiments) and compare two prints (`forca_academy_compare`),
  record an outcome, and write Markdown it created (lessons, playbooks, printer and material notes) or a starter file
  still exactly as Forca wrote it. It can't delete files or change `AGENTS.md` or a `record.json`.

## Constraints

- The folder belongs to the user: every Forca write either creates a missing file, appends at the user's request
  (a lesson), or replaces only a Forca-owned block, never the user's text. Forca never writes a lesson on its own.
- A widget-only line (like a description text) must not be added to a print settings page that object, part or plate
  tabs reuse: their option stripping assumes every line holds an option. Hints on the process page are added only on
  the main process tab.
