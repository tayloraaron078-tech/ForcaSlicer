#!/usr/bin/env bash
# Re-applies the Forca Slicer blue retint to OrcaSlicer's GUI chrome colors (POSIX/macOS/Linux).
#
# Mirrors apply_retint.ps1. Reads the canonical mapping from tools/forca/retint_map.tsv and
# the exclude list from tools/forca/retint_exclude.txt. Rewrites each grey #RRGGBB hex literal
# to its Forca-blue counterpart across src/slic3r/GUI/**/*.{cpp,hpp} and the three webview CSS
# files, skipping excluded files. Idempotent and order-independent.
#
# Usage:
#   tools/forca/apply_retint.sh --dry-run
#   tools/forca/apply_retint.sh
#
# EXCLUDED files (retint_exclude.txt) are hand-edited or hold functional recolor keys — do NOT
# retint them; re-apply their edits manually after a merge. See docs/forca/upstream-merge.md.
set -euo pipefail

DRY=0
[ "${1:-}" = "--dry-run" ] && DRY=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
MAP_FILE="$SCRIPT_DIR/retint_map.tsv"
EXC_FILE="$SCRIPT_DIR/retint_exclude.txt"
[ -f "$MAP_FILE" ] || { echo "Missing mapping file: $MAP_FILE" >&2; exit 1; }

# Build the perl substitution program and the exclude regex from the data files.
export MAP_FILE EXC_FILE

perl - "$REPO_ROOT" "$DRY" <<'PERL'
use strict; use warnings;
my ($root, $dry) = @ARGV;

# --- load mapping ---
my @pairs;
open(my $mf, '<', $ENV{MAP_FILE}) or die "open map: $!";
while (my $l = <$mf>) {
    $l =~ s/[\r\n]+$//;
    # A mapping line is exactly "#RRGGBB<ws>#RRGGBB"; everything else (incl. # comments) is ignored.
    if ($l =~ /^\s*(#[0-9A-Fa-f]{6})\s+(#[0-9A-Fa-f]{6})\s*$/) {
        push @pairs, [uc $1, $2];
    }
}
close $mf;
die "No mappings parsed\n" unless @pairs;

# --- load excludes (basenames) ---
my %excl;
if (open(my $ef, '<', $ENV{EXC_FILE})) {
    while (my $l = <$ef>) {
        $l =~ s/[\r\n]+$//; $l =~ s/^\s+|\s+$//g;
        next if $l eq '' || $l =~ /^#/;
        $excl{$l} = 1;
    }
    close $ef;
}

# --- gather target files ---
my @files;
my $gui = "$root/src/slic3r/GUI";
if (-d $gui) {
    # recurse for *.cpp / *.hpp
    my @stack = ($gui);
    while (@stack) {
        my $d = pop @stack;
        opendir(my $dh, $d) or next;
        for my $e (readdir $dh) {
            next if $e eq '.' || $e eq '..';
            my $p = "$d/$e";
            if (-d $p) { push @stack, $p; }
            elsif ($e =~ /\.(cpp|hpp)$/ && !$excl{$e}) { push @files, $p; }
        }
        closedir $dh;
    }
}
for my $css ('resources/web/homepage/css/dark.css',
             'resources/web/homepage/css/home.css',
             'resources/web/include/global.css') {
    my $p = "$root/$css";
    my ($base) = $css =~ m{([^/]+)$};
    push @files, $p if -f $p && !$excl{$base};
}

# --- apply ---
my ($total, $changed) = (0, 0);
for my $p (sort @files) {
    open(my $fh, '<:raw', $p) or next;
    local $/; my $text = <$fh>; close $fh;
    my $orig = $text;
    my $hits = 0;
    for my $pr (@pairs) {
        my ($old, $new) = @$pr;
        # match hex only when not part of a longer hex token
        my $n = ($text =~ s/(?<![0-9A-Fa-f])\Q$old\E(?![0-9A-Fa-f])/$new/gi);
        $hits += $n if $n;
    }
    if ($hits > 0) {
        (my $rel = $p) =~ s/^\Q$root\E[\/\\]?//;
        printf "  %4d  %s\n", $hits, $rel;
        $total += $hits; $changed++;
        if (!$dry && $text ne $orig) {
            open(my $out, '>:raw', $p) or die "write $p: $!";
            print $out $text; close $out;
        }
    }
}
print "\n";
if ($dry) { printf "DRY RUN: %d hex replacement(s) across %d file(s) would be applied.\n", $total, $changed; }
else      { printf "Applied %d hex replacement(s) across %d file(s).\n", $total, $changed; }
print "Reminder: hand-edited files are NOT touched by this script — see docs/forca/upstream-merge.md.\n";
print "Verify BitmapCache.cpp recolor keys are intact:  git diff -- src/slic3r/GUI/BitmapCache.cpp   (should be empty)\n";
PERL
