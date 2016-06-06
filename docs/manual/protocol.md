# Pull summary

The first step is to fetch the repository summary; this is similar to Git's info/refs file.
Unlike Git, the summary file can be GPG-signed.

The signature (summary.sig) is downloaded first, and the summary is skipped if the signature is up-to-date. If the summary has a signature, they are both cached in repo/summaries.

Next, the actual pull begins. This fetches the summary file again, for metalinks, to resolve the repository. There's a special fast path for local pulls. Otherwise, it errors if the format isn't archive-z2. It fetches the summary sig, and caches it, and then the summary. Also the config somewhere in there, and certificate/proxy handling.

If successful, it looks up the ref in the summary (or gets all refs for mirror mode). Otherwise it looks up the ref by url, refs/heads/XX.

It makes the state directory, and enters the fetch-objects state. It makes a queue of requested hashes. Then it pulls them concurrently, checking for static deltas if enabled. Commitpartials are written to the state directory and moved to the main repo when complete.

Metalink is using Walter's hand-written implementation, it could use https://launchpad.net/libmetalink instead
