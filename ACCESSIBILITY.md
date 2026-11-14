# Accessibility

What the Dragon GL client does for players who need larger text, more
contrast, less motion or the keyboard alone, and how each point is
checked. A point without a test is listed under *Known limitations*, not
here.

## What the client provides

| Need | What the client does | Checked by |
| :--- | :--- | :--- |
| Readable text | Every HUD text is drawn on an opaque panel or on its own dark plate, never straight on the scene. Each HUD colour has a contrast of at least 4.5:1 against the panel (WCAG 2.1, 1.4.3), over the darkest and over the brightest scene. | `client_ui` unit test (`test_contrast`): every colour of `hud_theme.c`, composited as the GPU does it |
| Larger or smaller interface | The HUD scales from 0.5x to 4x in steps of 0.25: automatically from the monitor scale (HiDPI) and the height of the window, with `--ui-scale`, with `ui_scale` in `client.conf`, or while playing with Ctrl+= and Ctrl+-. Glyphs are snapped to device pixels at every scale. | `client_ui` unit test (`test_ui_scale`); client test `test_settings_file_and_keys` (scale 2 under the validation layer) |
| Keyboard only | Everything is reachable from the keyboard: movement, the camera (Q, E, R, F, PageUp, PageDown), commands typed in the window with editing and history, the message log, the list of keys (F1). The mouse is never required. | client test `test_command_line_in_the_window` |
| Other keys | Every action can be moved to another key, two keys per action, and any key can send a command (`client.conf`, written by `--print-config`). A line the client does not understand is reported with its number; the rest applies. | `client_ui` unit test (`test_key_names`, `test_config_file`); client test `test_settings_file_and_keys` |
| Not colour alone | Hit points are a bar, a colour and the numbers; hunger and thirst, conditions, spell slots and equipment are words; a defeated boss has a star as well as a colour; messages keep their `[TAG]`; the compass uses letters. | review of `hud.c` (no test) |
| Less motion | `reduce_motion = yes`: no particle effects, combat numbers stay where they appear, the water does not wave. Nothing in the client flashes. | the setting: `client_ui` unit test (`test_config_file`); its effect: review |
| No window | The game can be played from the terminal that started the client: every message is printed there and commands can be typed there. A screen reader that reads the terminal reads the game. | client tests (they read the answers of the server from the terminal output) |

## Known limitations

- The 3D scene itself is not described in text beyond what `look` and
  `examine` answer; a blind player depends on those commands.
- The HUD font is a 5x7 bitmap with ASCII only: it has no bold or
  alternative typeface, and characters outside ASCII are shown as `?`.
- The colours of the scene (tiles, monsters) are not checked for contrast
  and there is no colour-blind palette for the minimap.
- The client has no sound, so nothing is conveyed by sound alone, and
  nothing can be.
- No conformance with WCAG as a whole is claimed: the standard is written
  for web content, and only the criteria named above were checked.

## Reporting a barrier

Open an issue on GitHub with the feature concerned, what you expected,
what happened, your operating system and the assistive technology or
input method you use. Do not include personal or sensitive information in
a public report.
