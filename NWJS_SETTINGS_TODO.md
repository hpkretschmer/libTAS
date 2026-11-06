# TODO: turn the NW.js environment variables into libTAS settings

Two features for NW.js/browser games are currently switched on with environment variables of the game (File → Executable Options → Environment variables), not with libTAS settings:

| Variable | Feature | Code |
| --- | --- | --- |
| `LIBTAS_PAGE_FRAMES=1` | Only end a frame when the page signals one (instead of on every present), from the start of the game | `src/library/PageFrame.cpp` (`enabledFromStart()`, `enabled`) |
| `LIBTAS_CASE_INSENSITIVE=1` | Retry missing file paths ignoring case | `src/library/fileio/CasePath.cpp` (`CasePath::enabled()`) |

Environment variables were the quickest way to get them working: they need no changes to the settings struct, the GUI or the config files. Real settings would be easier to discover and would be stored with movies. These are the steps to turn them into checkboxes, modeled on the existing "Prevent writing to disk" option (`prevent_savefiles`), which goes through every place involved. Search for `prevent_savefiles` to find each one.

## 1. Add the settings to `SharedConfig`

In `src/shared/SharedConfig.h`, next to `bool prevent_savefiles`, add:

```cpp
/* Only end a frame when a browser-based game signals one (see PageFrame.h) */
bool page_frames = false;

/* Retry missing file paths ignoring case (see fileio/CasePath.h) */
bool case_insensitive_paths = false;
```

`SharedConfig` is sent as raw bytes from the program to the library (`MSGN_CONFIG`), and its size is checked (`MSGN_CONFIG_SIZE` in `src/library/main.cpp`), so the program and the library must both be rebuilt.

## 2. Load and save them in the program's config

In `src/program/Config.cpp`, there are three places for `prevent_savefiles`; add both new settings in each, with the same pattern:

- the key parser used by `--set KEY=VALUE` (`else if (key == "prevent_savefiles") ...`), with keys `page_frames` and `case_insensitive_paths`
- saving (`settings.setValue("prevent_savefiles", ...)`)
- loading (`sc.prevent_savefiles = settings.value("prevent_savefiles", ...).toBool()`)

## 3. Add the checkboxes to the settings window

In `src/program/ui/settings/RuntimePane.h` and `.cpp`, follow `writingBox` (the "Prevent writing to disk" checkbox):

1. Declare two `ToolTipCheckBox*` members, e.g. `pageFramesBox` and `caseInsensitiveBox`.
2. Create them (`new ToolTipCheckBox(tr("..."))`), e.g. "Browser game frames (NW.js)" and "Case-insensitive file names", and add them to a layout of the pane.
3. Connect `clicked` to `RuntimePane::saveConfig`.
4. Write a description with `setDescription(...)`, explaining when to use them (games on NW.js, into which libTAS then injects its page script `libtas_web.js`; games made for Windows that access files with the wrong case).
5. In `loadConfig()`, `setChecked(context->config.sc.page_frames)` (and the other one); in `saveConfig()`, store `isChecked()` back.

Both only take effect when the game starts. Check how the pane handles other options that can't change while the game is running (look for `setEnabled` depending on `context->status` in the settings panes), and do the same.

## 4. Apply them in the library

The library receives the config during its initialization, in the `while (message != MSGN_END_INIT)` loop of `src/library/main.cpp` (`case MSGN_CONFIG`). Both features currently read their variable earlier (`PageFrame.cpp` at static initialization; `CasePath::enabled()` caches the value on its first call), so they must be switched on after the config is received:

- `PageFrame`: add a function, e.g. `PageFrame::enable()`, that sets `enabled` to true, and call it after the initialization loop in `main.cpp` if `Global::shared_config.page_frames` is set. Keep the existing behavior that the first page signal also enables page frames.
- `CasePath`: call `CasePath::enable()` (already exists) after the initialization loop if `Global::shared_config.case_insensitive_paths` is set. `CasePath.cpp` must keep not depending on the rest of libTAS, because the standalone library `libtas-casefold.so` (`src/library/casefold/casefold.cpp`) compiles it too and enables it in its constructor.

Then decide about the environment variables: remove them (and update the docs, step 6), or keep them as an alternative that also enables the features. Keeping them avoids breaking existing setups.

## 5. Store them in movies

Both settings change how a movie plays back (page frames change what a frame is), so a movie should carry them. In `src/program/movie/MovieFileHeader.cpp`, store and load them like `mouse_support`: `config.setValue("page_frames", ...)` in the save function, and `context->config.sc.page_frames = config.value("page_frames").toBool()` inside `if (!skipLoadSettings)` in the load function. Movies without the keys load them as `false`.

## 6. Update the documentation

- `docs/guides/options.md`: add a section for each checkbox, like "Prevent writing to disk".
- The OMORI setup guide and README in the [Omori-libTAS-sync](https://github.com/hpkretschmer/Omori-libTAS-sync) repository tell users to set `LIBTAS_PAGE_FRAMES` and `LIBTAS_CASE_INSENSITIVE` as environment variables; change them to the checkboxes.
- The comments at the top of `src/library/PageFrame.h` and `src/library/fileio/CasePath.h` mention the variables.

## 7. Test

1. Build, start the libTAS GUI, toggle both checkboxes, close it, and check that `~/.config/libTAS/<game>.ini` contains `page_frames` and `case_insensitive_paths`.
2. Remove the environment variables from the game's executable options (if they were removed in step 4), enable the checkboxes, and run an NW.js game (e.g. OMORI, see the Omori-libTAS-sync guide): the frame counter only advances with the game's frames, and the game finds files with the wrong case.
3. Record a short movie, check that its `config.ini` contains both keys, and play it back.
