#!/bin/sh
# Packs sounds/weakauras into sounds/weakauras.zip, the archive the addon downloads from this repository's main
# branch on its first load (run from the repository root after changing the sounds).
set -e
rm -f sounds/weakauras.zip
cd sounds/weakauras && zip -q -X ../weakauras.zip *.ogg *.wav CREDITS.txt
