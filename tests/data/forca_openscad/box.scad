// Forca test fixture: a small parametric box with every kind of Customizer parameter.

/* [Size] */
// Outside width
width = 40; // [10:0.5:100]
// Outside depth
depth = 30; // [10:1:100]
height = 15;

/* [Style] */
// Round the corners
rounded = true;
// Which part to render
part = "box"; // [box:Box, lid:Lid]
// Text on the lid ("quotes" and \ backslash)
label = "Forca";
// Lid size, width and depth
lid_size = [40, 30];

/* [Hidden] */
wall = 2;

module body() {
    difference() {
        cube([width, depth, height]);
        translate([wall, wall, wall]) cube([width - 2 * wall, depth - 2 * wall, height]);
    }
}

if (part == "box")
    color("#3D6B9C") body();
else
    color("#2B2B2B") cube([lid_size[0], lid_size[1], wall]);
