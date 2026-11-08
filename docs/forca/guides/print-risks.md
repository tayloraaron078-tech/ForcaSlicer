# Print Risks

**View > Print Risks...** lists the places on the sliced plate where the print may fail, before you send it. Forca
works it out from the slice itself, layer by layer, so it needs the plate sliced first.

## What it finds

Forca compares each layer with the one below it:

- **Overhang:** a part that reaches farther past the layer below than the support threshold angle allows (Support >
  Threshold angle; 30° when it is 0). It may curl up or droop.
- **Long bridge:** a bridge longer than **Max bridge length** (Support > Advanced). Long bridges sag.
- **Floating island:** a part that starts in mid-air, with nothing printed under it.

Each row shows the object, the height (one layer or the first and last layer of the risk), its size (how far the
overhang reaches, the bridge's span, the island's area) and whether support is printed under it.

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
and filament were tagged **Bridging** (for a bridge) or **Supports** (for the others), with **Show** to list them,
and **Open the ... playbook** when the Academy folder has that playbook.

## Elsewhere

- **Before you print:** when you send the plate, the send dialog says how many places may fail with no support under
  them, and points to View > Print Risks.
- **Forca AI:** an AI assistant you connected can read the same list (the `forca_risk_report` tool) to explain the
  risks and suggest fixes. See [Forca AI](forca-ai.md).

Print Risks is guidance, not a guarantee: a print can fail for reasons a slice doesn't show (adhesion, cooling,
the filament), and a listed risk may print fine on your printer. It covers FFF prints only.
