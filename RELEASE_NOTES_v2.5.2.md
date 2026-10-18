# OBS AirPlay v2.5.2

A security and stability hotfix. **Please update.** Version 2.5.1 and earlier can be crashed by a single request from any device on the local network: no pairing and no AirPlay session are needed. Version 2.5.2 closes that hole and fixes the hangs and the rename crash listed below. It has no new features.

## Security fixes

- **A LAN request can no longer crash OBS.** In 2.5.1 and earlier, an empty or truncated `POST /fp-setup2` request made the plugin read through a missing body, which crashed OBS. Such requests are now answered `400 Bad Request`.
- **One stalled connection no longer freezes the receiver.** A connection that sent a few bytes and then stopped could hold up the receiver's single network thread, blocking screen mirroring and AirPlay video for every device. The receiver now reads each request without blocking (ported from upstream UxPlay). It also closes a connection that stalls part-way through a request, or never identifies itself, after 60 seconds.
- Several checks that only existed in debug builds are now real checks in release builds. A request whose `X-Apple-Session-ID` does not match its connection is rejected, and running out of playlist slots or memory on `POST /play` is reported as an error. Neither can crash the plugin any more.

## Stability fixes

- **Renaming the receiver no longer risks a crash.** The receiver switches to the new name before the old advertisement is removed. Requests that arrive during a rename no longer read freed memory.
- **Stopping AirPlay video can no longer hang OBS.** A stop that arrived just as an item paused could hang the network thread, or OBS at quit. Play and stop requests that arrive at the same moment from different parts of the plugin are now handled one at a time, so they can no longer crash OBS.
- After a failed frame allocation in the software video decoder, the next frame retries the allocation. Before, it wrote into missing buffers.
- If you typed a character in the server name and deleted it again within the 800 ms delay, the advertised name could end up different from the setting. The advertised name now always matches what is in the box.

## Updating

- **From v2.4.0 or later:** click **Install Update** in the prompt, then quit OBS when it suits you. The plugin is replaced as OBS quits.
- **From v2.3.0 or earlier:** close OBS Studio, download the Apple silicon `.pkg` installer, run it, and reopen OBS.

## Compatibility

- macOS 12 Monterey or later
- OBS Studio 28 or later
- Apple silicon (`arm64`)
