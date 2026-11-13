# CarLink

Start with README.md for the current status; docs/history/codex-handoff-log.md has the detailed early milestones.

- Treat references/ and samples/ as read-only fixtures. Keep firmware, captures and any credentials local.
- Keep Android Auto and factory CarPlay recoverable. Continue offline until a specific supervised head-unit test is authorized.
- Never send mdev data while another projection context is attached to the same input. Confirm the target and the cleanup path before each live test.
- Preserve the distinction between mdev acceptance, a Screen POST and a verified visible image. Reuse the existing results instead of rerunning prior probes.
