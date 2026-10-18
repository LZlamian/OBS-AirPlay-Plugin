# OBS AirPlay v2.5.0

One new option, for apps that send AirPlay video and also play the same video on the device.

## What's new

- **Keep AirPlay video in step with the sending app.** A new checkbox in the AirPlay source's properties, off by default. With it on, AirPlay video is timed from the moment the sender's play, seek or resume command arrives and the position it names. The pictures that became late while the file was opened, or while the frame was found, are skipped.

  Without it, playback starts at the named position whenever the first picture is ready, so OBS stays behind the device by the time that took (0.2 to 1 second on a slow connection) until the next command. That is right for Safari and other senders that wait for the receiver, and wrong for an app that keeps its own player running, where OBS then shows the video later than the device does.

  In testing with such an app, each start, chapter jump and resume began within 0.05 seconds of the sender.

- Still clips and very short items always show a picture with the option on, also when they open slowly: a frame counts as late only once it has ended.
- Video and sound catch up separately, so sound that is ready first does not make the skipped pictures flash by.

## When to turn it on

Turn it on when the sending app plays the video on the device at the same time and OBS should match it. Leave it off for Safari, the Photos and TV apps and other senders that follow the receiver: with it on, they would lose the first moment of each start.

The option applies to AirPlay video only. Screen mirroring is not affected.

## Diagnostics

- With the option on, the OBS log has one line per start, seek or resume: `[MEDIA] in step with the sender: picture at …s, sender at …s, … ms after its command (… frames skipped)`.

## Updating

- **From v2.4.0 or later:** click **Install Update** in the prompt, then quit OBS when it suits you. The plugin is replaced as OBS quits.
- **From v2.3.0 or earlier:** close OBS Studio, download the Apple silicon `.pkg` installer, run it, and reopen OBS.

## Compatibility

- macOS 12 Monterey or later
- OBS Studio 28 or later
- Apple silicon (`arm64`)
