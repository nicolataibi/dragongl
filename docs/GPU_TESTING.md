# Testing the client on a real GPU

CI runs the client on Mesa lavapipe, a software renderer, under the
validation layer: that proves the Vulkan calls are valid and that the
window, the HUD and the input work. It does not prove anything about a
real driver, a real monitor or the frame rate. This checklist is what
remains to be done by hand, once per release, on each kind of GPU at
hand (AMD, Intel, NVIDIA; X11 and Wayland).

Start a server (`./dragongl-server`), then the client with the
validation layer and keep its terminal:

```bash
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ./dragongl-client 127.0.0.1 2>&1 | tee client.log
```

| # | Do | Expect |
| :-- | :--- | :--- |
| 1 | Read the start of the log | The GPU in use is the one you expect (`DRAGONGL_GPU` chooses); `[VK] Enabling 1 layer(s)` |
| 2 | Walk in town, go down a few floors, fight, cast a spell | Scene, minimap, particles and combat numbers are drawn; no flicker, no missing tile |
| 3 | Resize the window by dragging for ten seconds; maximize; full screen if the compositor offers it | The picture follows; the HUD grows and shrinks with the height of the window (unless `ui_scale` is set); no crash |
| 4 | Minimize for ten seconds, restore | The game is still there |
| 5 | Shrink the window under 1000 pixels of width | Equipment and trophies disappear; Tab brings them back |
| 6 | On a HiDPI monitor (or with the desktop scale at 150% or 200%) | HUD text is as large as the text of the desktop, and sharp; moving the window to a monitor with another scale adapts it |
| 7 | Ctrl+= four times, Ctrl+- four times; `--ui-scale 0.5` and `--ui-scale 4` | Sharp glyphs at every size; panels never leave the window |
| 8 | Enter, type a long command, edit it with the keys of F1, send it; Up; Shift+PageUp | As described by F1; the answer of the server is in the message panel |
| 9 | Switch to a text console and back (Ctrl+Alt+F3, Ctrl+Alt+F1 or F2); suspend and resume the machine | The game goes on; if the log says `Device or surface lost: restarting the renderer`, it must be followed by `Renderer restarted.` |
| 10 | `DRAGONGL_TEST_DEVICE_LOST=300 ./dragongl-client 127.0.0.1` | After a few seconds the renderer restarts once and the game goes on |
| 11 | `DRAGONGL_VSYNC=0 ./dragongl-client --frames 2000 127.0.0.1` on floor 0, then press F3 while playing in a deep floor | `[VK] Frames: 1999, frame time p50 ... p99 ... max ... ms`: p99 under 16.7 ms on a discrete GPU, under 33 ms on an integrated one |
| 12 | Escape; then `grep -c -e VUID- -e "Validation Error" client.log` | The client exits with status 0; the count is 0 |

Report a failure with the first lines of the log (GPU, driver), the row
of the table, and the validation messages if any.

The budgets of row 11 are targets, not measurements: no frame time has
been measured on a real GPU yet. Write what you measure in the issue or
in `DEVELOPMENT.md`, section 2.
