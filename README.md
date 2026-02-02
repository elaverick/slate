[![Build and Release (Win32)](https://github.com/elaverick/slate/actions/workflows/build-and-release.yml/badge.svg)](https://github.com/elaverick/slate/actions/workflows/build-and-release.yml) ![GitHub Release](https://img.shields.io/github/v/release/elaverick/slate)


# Slate

A calm, liminal tool for text — built for large files, quiet focus, and the spaces in between.

This editor is intentionally simple. It’s designed to sit somewhere between a scratchpad and a file editor: fast to open, comfortable to keep running, and capable of handling very large text files without ceremony. There are no projects, tabs, or IDE features — just a persistent space for reading, writing, and making small, deliberate changes without friction or distraction

## Features

- File operations: New, Open, Save, Save As, Exit
- Edit functions: Undo, Redo, Cut, Copy, Paste, Delete, Select All
- Right-click context menu with edit operations
- Find function: Search within the document, with forward/backward direction, match case, and Find Next.
- Word Wrap toggle: Switch wrapping on or off for long lines.
- Show Whitespace toggle: Reveal/hide spacing and non-printable characters.
- Theme toggle: Flip between Slate’s palette and system colors.
- Command mode: Vim-style
  - Save (:w)
  - quit (:q)
  - write-and-quit (:wq)
  - open file (:e <file>)
  - search (:s with direction/case options).
- Help and About dialogs
- Status bar showing:
  - Current line and column position
  - Insert/Overwrite mode indicator
  - Caps Lock indicator
  - Whitespace display mode


## Development process

This project was developed experimentally with the assistance of AI coding tools.
It reflects an iterative, exploratory process rather than strict manual authorship.
The code is shared as-is, without claims of originality beyond its present form.

## Building

To build the application, run the build script:

```cmd
build.bat