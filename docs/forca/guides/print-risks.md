# Print Risks

**View > Print Risks...** lists the places on the sliced plate where the print may fail, before you send it. Forca
works it out from the slice itself, layer by layer, so it needs the plate sliced first.

## What it finds

Forca compares each layer with the one below it:

- **Overhang:** a part that reaches farther past the layer below than the support threshold angle allows (Support >
  Threshold angle; 30° when it is 0). It may curl up or droop.
- **Long bridge:** a bridge longer than **Max bridge length** (Support > Advanced). Long bridges sag.
- **Floating island:** a part that starts in mid-air, with nothing printed under it.
- **Thin and tall:** a part narrower than 8 mm that stands more than 8 times as tall as it is wide. The nozzle can
  make it wobble or knock it off.
- **Small footprint:** an object at least 10 mm tall whose first layer is under a fifth of its largest layer, or whose
  base is more than three times narrower than the object is tall. It may come loose or tip over; a brim or raft
  takes care of it.
- **Short layers:** layers that print in less than 90% of the filament's layer time (Cooling > Layer time), so they
  may not cool. These come from the G-code, so they appear once the plate has finished slicing, as **Whole plate**,
  without a marker.

Each row shows the object, the height (one layer or the first and last layer of the risk), its size (how far the
overhang reaches, the bridge's span, the island's area, the part's width, the first layer's area, the shortest layer
time) and whether support is printed under it (for a small footprint: whether a brim or raft holds it; a dash where
support doesn't apply).

## Using the list

- **Select a risk** to see what to try (turn on support, turn the model, Thick external bridges, a lower bridge
  speed, ...). Forca switches to the Preview and moves the layer slider to where the risk starts.
- **Numbered markers** on the Preview show each risk as the layer slider reaches it. Hover a marker for its type and
  size, click it to select its row. Untick **Show numbered markers on the Preview** to hide them.
- The window stays open while you work. **The list follows each new slice** of the plate, and of another plate when
  you switch to it.
- A risk with support under it is usually fine. "Support is on, but none is printed under it" points at **On build
  plate only**, support blockers or the threshold angle.

With [Forca Academy](forca-academy.md) on, a selected risk also shows how many of your journal prints with this printer
and filament were tagged **Bridging** (for a bridge), **Bed adhesion** (a small footprint), **Spaghetti** (a thin tall
part) or **Supports** (overhangs and islands), with **Show** to list them, and **Open the ... playbook** when the
Academy folder has that playbook.

## Plate report

**Plate report** (in this window, under **View > Plate Report...**, and from **Before you print**) shows the whole
plate on one page, by fixed rules only:

- **Checks:** objects outside the printable area or within 5 mm of its edge, taller than (or within 10% of) the
  printable height, a layer or first layer thicker than 80% of the nozzle, a nozzle temperature outside the filament's
  recommended range, filaments far apart in nozzle (over 30 °C) or bed temperature (over 15 °C), and, once sliced, the
  slicer's own warnings (such as a bed hotter than the filament's softening temperature).
- **Print Risks:** how many, of which kind, and how many have nothing taking care of them.
- **Print:** print time, filament weight and length, cost, layers and filament changes (once sliced).
- **Plate:** size, position against the bed's centre, the bed and its type, and the number of objects.
- **Presets** and the settings that most often decide a print, and every setting **changed and not saved**.

**Copy as text** puts the report on the clipboard, to keep or share.

## Elsewhere

- **Before you print:** when you send the plate, the send dialog says how many places may fail with nothing taking
  care of them, points to View > Print Risks, and counts the plate report's warnings, with a link to the report.
- **Forca AI:** an AI assistant you connected can read the same list (the `forca_risk_report` tool) and the plate
  report's checks (in the slice result) to explain the risks and suggest fixes. See [Forca AI](forca-ai.md).

Print Risks is guidance, not a guarantee: a print can fail for reasons a slice doesn't show (the bed, the filament,
the room), and a listed risk may print fine on your printer. It covers FFF prints only.
