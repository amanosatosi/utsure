# Credits

Utsure includes third-party media assets used for queue completion and failure notifications.

## Zundamon character artwork

Character: ずんだもん (Zundamon)

Zundamon is a character from the 東北ずん子・ずんだもんプロジェクト
(Tohoku Zunko / Zundamon Project), operated by SSS LLC.

Character project:
https://zunko.jp/

The following illustrations are created by すだちみのり and distributed through
すだちみのり工房:

- `ずんだもんー猫だっこ1.png`
  - Bundled as `notification-success-zundamon.png` for Windows build-tool compatibility.
  - Used for successful queue completion notifications.
  - Source: 「猫とずんだもん」

- `ずんだもんー落ち込む12.png`
  - Bundled as `notification-failure-zundamon.png` for Windows build-tool compatibility.
  - Used for failed queue notifications.
  - Source: 「膝を抱えて落ち込むずんだもん」

Illustrator:
すだちみのり

Illustration source:
https://sudachiminori.com/

Illustration usage terms:
https://sudachiminori.com/利用規約/

These illustrations are used in accordance with the すだちみのり工房 usage
terms and the 東北ずん子・ずんだもんプロジェクト guidelines.

The copyright of the illustrations remains with すだちみのり.
The Zundamon character and related character rights belong to their respective
rights holders.

## Notification sound effects

Notification sound effects are provided by 効果音ラボ (Sound Effect Lab).

Source:
https://soundeffect-lab.info/

Original source assets:

- `決定ボタンを押す41.mp3`
  - Used for successful queue completion notifications.

- `ビープ音4.mp3`
  - Used for failed queue notifications.

Utsure bundles PCM signed 16-bit little-endian, 48 kHz, mono WAV conversions
of these sound effects for notification playback:

- `notification-success.wav`
  - Qt resource alias: `決定ボタンを押す41.wav`

- `notification-failure.wav`
  - Qt resource alias: `ビープ音4.wav`

The audio format conversion does not change the ownership or usage terms of
the original sound effects.

The sound effects remain subject to the 効果音ラボ usage terms.

Usage terms:
https://soundeffect-lab.info/rules/
