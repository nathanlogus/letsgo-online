# Repository agent instructions

Read and follow [CLAUDE.md](CLAUDE.md) before working in this repository. It contains the shared build, testing, save-copy and artifact conventions. The workflow below supplements those instructions and makes isolated automated debugging the default.

## Implement, reproduce and verify

- For game/runtime changes, reproduce the relevant behavior, implement the fix and run appropriate automated checks yourself. Do not stop at a proposed fix or ask the user to perform routine testing when existing automation can do it.
- Prefer extending existing run scripts and test tools. Inspect `tests/scripts/` and `tests/tools/` before creating another harness. If they are unavailable in a fresh checkout, recreate the necessary harness under `tests/`.
- Use scripted input, copied-save fixtures, logs and captures to exercise the actual game when relevant. Inspect the captured output; a successful build or a log message alone does not establish correct visible behavior.
- For co-op changes, exercise both clients and relevant timeout/disconnect paths. For save-affecting changes, compare the appropriate before/after state, including unintended changes and duplicate rewards where applicable.
- Run checks appropriate to the change. Documentation-only edits do not require launching the game. Distinguish verified behavior, controlled prototype results and remaining gaps; do not describe partial state comparisons as complete synchronization proof.

## Isolate every automated run

- Keep all generated test artifacts under Git-ignored `tests/`, following `CLAUDE.md`. Give each run a unique directory such as `tests/runs/<feature>-<scenario>-<id>/`; never overwrite a previous run.
- Use independent save copies, settings, logs, capture paths and relay ports for each test client. Never point scripted tests at the player's own save or edit `build/larecomp.ini`.
- Inspect running processes before building or launching. Do not stop, replace or send scripted input to a user's manual game session. Track the exact executable paths and process IDs of your own runs; clean up only those processes when finished, preserving their artifacts.
- When the normal build is in use, or when testing experimental runtime changes, build into an isolated directory. For example, from the configured MSYS2 shell:

  ```bash
  make -j8 B=tests/build-qa tests/build-qa/LetsGoRecomp.exe
  ```

- Keep the executable filename `LetsGoRecomp.exe`: the extracted game DLL imports it. Use `LARECOMP_DATA_ROOT` pointing at the repository's `build` directory and include that directory in PATH for runtime DLL dependencies. Keep per-run save/settings/capture overrides separate. Runtime-only changes normally do not require rebuilding the extracted game DLL.
- Limit concurrent game instances to what the machine can sustain. Separate GPU stalls from network latency before drawing performance conclusions.

## Preserve work and communicate evidence

- Preserve unrelated working-tree changes and original/manual saves. Do not reset, stash, delete or overwrite other work to simplify testing.
- During extended debugging, provide concise progress updates at least once per minute, stating what was learned and what the next check resolves.
- Report what changed, which checks passed, the relevant artifact paths and material limitations. Leave enough evidence for another session to resume without repeating the investigation.
- For ongoing co-op work, consult [the roadmap](docs/COOP-ROADMAP.md), [the battle prototype notes](docs/COOP-BATTLE-PROTOTYPE.md) and [the handoff](docs/COOP-AGENT-HANDOFF.md). Reconcile historical status text with newer evidence and current code. Independent saves and Social mode remain the default; do not silently apply cross-save progression.
