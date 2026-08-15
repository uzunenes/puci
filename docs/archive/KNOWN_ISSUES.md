# Known Issues and Deferred Findings

The following items are known limitations and deferred findings tracked for future milestones:

- on `PUCI_ERR_BUFFER_TOO_SMALL`, `*out_count` reports what was written, not what was required, forcing a second `cap == 0` call
- unanswered: whether the memory-safety fixes can be split out of the release commit, and why `heartbeats >= 3` was retained alongside the new structural assertions
