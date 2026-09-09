# M8 independent review

Date: 2026-09-09. Reviewer: Astra/max through the approved astra-orchestrator workflow.
The reviewer worked read-only; the root integrated fixes and ran verification.

Resolved findings:

- Match distribution project, startup scene and SDK identities; reject ambiguous executables.
- Keep staging outside copied inputs and reject reparse points; preserve no-overwrite publication.
- Verify the complete file/executable inventory and treat timeout/helper errors as failures.
- Preserve the lifetime of the asset catalog used for copy/Redo checks.
- Require an explicit Stop request and record native Export button coordinates for M8.
- Give compiler children the selected GCC directory while preserving the parent environment.
- Use an existing short path for validation-layer discovery and require active Debug validation.

Final code verdict after follow-ups: no remaining material findings in the reviewed changes.
The validation-path workaround remains conditional on a usable short alias.
Test execution and the final delivered file inventory are recorded separately in M8_REPORT.md
and m8-summary.json; review alone is not a substitute for those runs.
