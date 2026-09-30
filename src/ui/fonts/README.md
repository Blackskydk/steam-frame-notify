# Bundled fonts

`Inter-Regular.ttf` and `Inter-SemiBold.ttf` are unmodified copies of Inter 3.019
(https://github.com/rsms/inter), which is licensed under the SIL Open Font License 1.1. The license
text is in [OFL.txt](OFL.txt); both font files also carry the copyright and license in their `name`
tables. Inter has no Reserved Font Name.

Frame Notify loads them at runtime (see `src/ui/typography.cpp`). The build copies this directory
next to the executable as `fonts/`; if the files cannot be found the UI falls back to a built-in
bitmap font and prints a warning. To use another location, set `FRAME_NOTIFY_FONT_DIR`.
