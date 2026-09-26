# Testing Forca Slicer — how to help

Thank you for trying the alpha. The most useful thing you can do is **use it for real prints on your printer and
report what happens** — good or bad. A report of "it worked on my Prusa MK4 with PETG" is valuable too.

## Before you start
- Keep your normal slicer installed. Forca uses its own settings folder; your OrcaSlicer setup isn't touched.
- Start with small, cheap prints. Check the preview and watch the first layers.
- Note your Forca version (Help → About).

## What to try (pick what interests you)
1. **Everyday slicing:** slice a few of your usual models with your usual profiles. Does anything differ from
   OrcaSlicer (time, look of the preview, the print)?
2. **Regional support interfaces:** a model with an overhang you want to peel cleanly. Add a support-interface
   modifier (right-click the object → **Add Support Interface Modifier** → pick a shape), or use the
   support-interface paint tool;
   choose a different interface filament (for example PETG under PLA). Slice, check the preview colors, print.
3. **Calibration Wizard:** calibrate one filament end to end (Calibration Wizard tab). Did each step make sense?
   Did the result improve your prints?
4. **Forca AI (only if you use an AI assistant like Claude Code):** turn it on in the Forca AI window, connect your
   assistant, and ask it to prepare a print. Tell us what it got right and wrong.

## How to report
Use the forms at <https://github.com/tayloraaron078-tech/ForcaSlicer/issues/new/choose>:
- **Bug report** — something wrong or crashing.
- **Printer report** — "Forca on my printer": worked / didn't, even with no bug.
- **Feature request** — ideas.

Helpful to include:
- What you did, what you expected, what happened (photos of the print help a lot).
- Printer model and nozzle, filament(s), support type.
- The project file (`File → Save project as`) if you can share the model.
- **Logs:** Help → **Troubleshoot Center** → *Stored logs* → **Pack…** saves all of Forca's logs as one
  `ForcaSlicer_Logs_….zip` to attach. (The **Pack** button at the top also adds the open project file — only use it
  if you're happy to share the model.) Logs are plain text. When a printer was connected they include its **serial
  number** and your **local network addresses**, and file paths include your **Windows user name**. They don't
  contain passwords or printer access codes, but GitHub issues are public — open the files first and replace
  anything you don't want to share (a find-and-replace in Notepad is enough).

Please report Forca problems **here, not to OrcaSlicer**, even if you suspect the bug is Orca's.

## Questions, not bugs
Use [Discussions](https://github.com/tayloraaron078-tech/ForcaSlicer/discussions).
