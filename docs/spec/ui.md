# Voice app UI

This page describes the voice app that runs from the `ota_1` partition (`firmware/src/main.cpp`). The launcher menu in `ota_0` is the upstream M5StopWatch-UserDemo and is not covered here.

All on-screen text of the voice app is Japanese, except the key tips.

## Screens

| Screen | When it is shown | What it shows |
|---|---|---|
| Idle eyes | Default. Shown from power-on; Wi-Fi and BLE connect in the background. | Two white rounded eyes at full size in the centre. They blink, glance around and react to touch and motion. |
| Listening | While the yellow key is held and the audio is uploaded. | Eyes at 85 % size, raised by 45 px. A voice level bar under the eyes (only while recording). The label `聞き取り中`. The transcript so far in light blue below it. |
| Thinking | After release, until the first character of the reply arrives. | The label `考え中…`. The eyes look up and to the right. The final transcript in light blue. |
| Reply / history | While the reply is typed out, for 15 s after it, or when opened with a tap on the yellow key. | The conversation as a chat: the user's words right-aligned in light blue, replies left-aligned in white. Small eyes (20 % size) at the top. |
| Offline | For 2.5 s after a failed attempt to reach the relay, or for as long as the yellow key is held while offline. | "x" eyes and a one-line message in pink under them. |

### Reply / history layout

| Item | Value |
|---|---|
| Text area width | 74 % of the screen width, centred |
| Text area height | from y = 80 to the screen height minus 80 |
| Maximum line width | 85 % of the text area |
| Gap between turns | 22 px |
| Typing speed | 40 characters per second, updated at most every 100 ms |
| Turns kept on the watch | 20 (oldest dropped first) |
| Text canvas height | 4000 px; the oldest turns are dropped until the text fits |

- Content shorter than the text area is centred vertically.
- Taller content follows the newest line. Dragging scrolls it, and a dot on the right edge of the circle shows the position.
- A reply that has not started yet is shown as `…`.
- A failed reply is shown as a pink notice under the history instead of a turn.
- When a reply has been typed out completely, the watch vibrates for 60 ms and the history stays on screen for 15 s.
- The history is loaded from the relay once: when Wi-Fi first connects, when BLE is authenticated without Wi-Fi while the eyes are idle, or when the history is opened and has not been loaded yet. It holds the last 20 turns of the relay's current session.

### Offline messages

| Message | Trigger |
|---|---|
| `Wi-Fiにつながってないよ` | The yellow key is held longer than 0.4 s while neither Wi-Fi nor an authenticated BLE link is available. |
| `Macにつながらないよ` | While talking, the relay did not answer the start of the upload (or the BLE link failed) and at least 0.4 s has been recorded. Also when the upload result is not a success, and when a new session cannot be started. |

### Reply errors

These appear as a pink notice in the history.

| Message | Trigger |
|---|---|
| `Macとの接続が切れたよ` | The reply comes over BLE and the link has been down for 15 s. |
| `不明なエラー` | The relay's answer has no reply text. |
| Relay messages | The relay's own error text (see [relay.md](relay.md#claude-session)). |

`メニューが見つかりません` appears in the history when the blue key is held and `ota_0` holds no valid app.

## Inputs

### Keys

| Input | Condition | Action |
|---|---|---|
| Yellow key (KEY A), hold | Idle or history shown | Records while held, then sends the recording to Claude. Recording stops automatically after 20 s. |
| Yellow key, tap | Idle or history shown | Recordings shorter than 0.4 s are not sent. They open or close the history instead. |
| Yellow key, tap or hold | No Wi-Fi and no BLE | A press of 0.4 s or less opens or closes the history. A longer press shows the offline screen. |
| Blue key (KEY B), click | Idle or history shown | Starts a new session on the relay and clears the history on the watch. |
| Blue key, hold | Idle or history shown | Returns to the launcher menu (`ota_0`). The hold threshold is the M5Unified default of 500 ms. |
| Power key, click shorter than 2 s | Any time | Shows the key tips for 2.7 s. |
| Any key | Eyes asleep | Wakes the eyes. |

Both colour keys are ignored while a recording is being sent or a reply is in progress.

A recording in which the relay recognises no speech returns to the idle eyes without a message.

### Key tips

The tips appear next to the physical keys: yellow at the top left and blue at the top right with the screen upright, and at the bottom right and bottom left when the screen is flipped.

| Key | Line 1 | Line 2 |
|---|---|---|
| Yellow | `Hold: Talk` | `Tap: History` |
| Blue | `Tap: New session` | `Hold: Menu` |

The power key is read from the PMIC key status register every 40 ms, because M5Unified's `BtnPWR` does not report it on the StopWatch. Presses of 2 s or longer are ignored by the app.

### Touch

| Input | Screen | Action |
|---|---|---|
| Touch and move | Idle eyes | The eyes follow the finger. After release they return to the centre and resume glancing 1.5 s later. |
| Touch | Eyes asleep | Wakes the eyes. The touch does nothing else. |
| Drag up or down | History | Scrolls. Each touch keeps the history open for another 15 s. |
| Tap | History, idle | Closes the history. |
| Circles, strokes | Idle eyes | Expressions; see below. |

## Expressions

Expressions are detected only on the idle eyes with the history closed, and are drawn only on the full-size eyes.

| Expression | Trigger | Duration | Look |
|---|---|---|---|
| Surprised | One acceleration peak: the magnitude differs from 1 g by more than 1.2 g. Peaks closer than 80 ms count as one. Not while Angry is shown. | 1.5 s | Eyes 15 % wider and 22 % taller, rounder corners |
| Angry | Three acceleration peaks within 0.8 s (shaking) | 3 s | Inner upper corners cut away, eyes tremble |
| Dizzy | The finger goes round the centre of the screen two full turns (more than 720 degrees) without lifting, staying more than 50 px from the centre | 3 s | Turning spiral eyes, three chicks circling above |
| Happy | Stroking in the upper 40 % of the screen: two changes of horizontal direction within 1.5 s, each move longer than 6 px, vertical range at most 60 px. Not while asleep. | 3 s | Lower lids pushed up, pink blush, small bounce |
| Tired | Battery at 20 % or less and not charging. Checked every 30 s. | While the condition lasts | Upper 45 % of the eyes covered by drooping lids (only with no other expression), slower and lower glances |

Surprised and Angry also wake the eyes.

### Idle motion

| Motion | Timing |
|---|---|
| Blink | 170 ms. The next blink comes 2.2 to 5.7 s later; one in five comes 2 s sooner (double blink). |
| Glance | Every 1.2 to 4.0 s, mostly near the centre and sometimes further out. Tired: every 3.0 to 7.0 s, low and slow. |
| Following the finger | Gaze up to 110 px horizontally and 100 px vertically at the edge of the screen. |

## Sleep

| Item | Value |
|---|---|
| Falls asleep after | 30 s without activity on the idle eyes |
| Sleeps for | a random time between 60 s and 600 s, then wakes by itself |
| Wakes on | touch, yellow or blue key, power key click, Surprised or Angry |
| While asleep | eyes close to a line, look slightly down, breathe slowly, "Z" letters drift up to the upper right |
| Brightness | 40 while asleep, 150 otherwise |
| Frame interval | 100 ms while asleep |

The eyes do not fall asleep while the history is shown or a conversation is in progress.

## Screen rotation

The screen turns 180 degrees so that text stays upright, based on the accelerometer.

| Item | Value |
|---|---|
| Axis | accelerometer x, sign inverted |
| Threshold | 0.45 g; between -0.45 g and 0.45 g the current side is kept |
| Hold time | the new side must persist for 500 ms |
| Flat screen | rotation is kept while the z axis reads more than 0.85 g in either direction |
| While recording | rotation is not updated |

Only rotation 0 and 2 are used.

## Font

| Item | Value |
|---|---|
| Main font | Noto Sans JP Medium, 24 px, converted into the firmware by `firmware/tools/make_font.py` |
| Characters in the main font | ASCII, JIS X 0208 rows 1 to 8 (symbols, digits, Latin, kana, Greek, Cyrillic, box drawing), JIS level 1 kanji, and a short list of extra characters in `make_font.py` |
| Fallback font | M5GFX built-in `lgfxJapanGothic_24` (JIS level 1 and 2) |

In the history, each character missing from the main font is drawn with the fallback font. Characters outside the Basic Multilingual Plane (4-byte UTF-8, such as emoji) are treated as missing and handed to the fallback font.

The transcript on the listening and thinking screens, the labels and the key tips use the main font only. Characters missing from it are not drawn there.
