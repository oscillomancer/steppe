# ADR-0078: Unsaved projects autosave and come back after a crash

**Status:** Accepted, 2026-11-14. Extends ADR-0014 (recording recovery) and
the autosave of ADR-0030/ADR-0076. Contracts: `docs/specs/session-model.md`
§5.3, `docs/specs/gui.md` §4.11, the `project.recoverable`,
`project.recover`, `project.recoverable.delete` and `project.get` commands.

## Context

Autosave covered saved projects only: an untitled project (the GUI's start
state) had none, so a crash lost everything but its recordings. Those
recordings stayed in the run's own folder, `<data>/<slug>/unsaved/<start
time>-<pid>/`, which nothing listed or offered back; `project.recoverable`
and `project.recover` only looked at the current project's `rec/`. The GUI
used neither, nor `autosave_available` for saved projects.

## Decision

1. **Untitled projects autosave into their own folder,**
   `<scratch>/autosave-<n>.json`, with the same writer (branches kept), the
   same trigger (60 s after a change of any branch head) and the same
   rotation (the newest 5) as saved projects. The first save deletes them
   with the rest of the folder; discarding the project deletes the folder.
2. **Clean close vs crash is an OS lock, not a pid.** Each run holds an
   exclusive lock on `<scratch>/session.lock` (`flock` on POSIX,
   `LockFileEx` on Windows) from its start for as long as its unsaved
   project uses the folder. The system releases it however the process
   ends. A folder beside the run's own whose lock can be taken belongs to a
   run that is gone; one whose lock is held belongs to a running app and is
   never listed or touched. Process IDs are reused, so they decide nothing.
3. **Nothing with content is deleted unasked.** A left-behind folder with an
   autosave or any other file stays until the user recovers or deletes it.
   Only folders with nothing in them (a run that crashed before its first
   edit) are removed when an app starts, and only if untouched for an hour
   (a run starting at the same moment may not hold its new folder's lock
   yet).
4. **Commands extend the recording recovery.** `project.recoverable` lists
   `sessions` beside `recordings`: each left-behind folder with when its
   run started, its newest readable autosave's time, project name, tracks
   and branches, its leftover recordings and its size on disk.
   `project.recover {session}` opens one: it replaces the current project
   (as `project.open`; the caller asks about unsaved changes first), from
   the newest autosave that reads (older ones are tried, with a warning),
   takes the folder over as its own (with its lock), stays unsaved, and puts
   back the folder's leftover recordings as one more undo step. Without a
   readable autosave it recovers only recordings that can really be put back
   (onto one new channel); with none, it refuses (`recover.damaged`) and
   changes nothing. An autosave whose branches were not restored, or one that
   could not be read, is kept: copied at recovery out of the rotation, and
   published beside the file by the first save (`<file>.pre-branches.bak`,
   `<file>.unreadable-autosave.bak`, never replacing) before the folder is
   released. (Amended 2026-11-14 after the recovery review: an empty `rec/`
   made a damaged session recover as a blank project, and the first save
   deleted the only raw copy of rejected branches.)
   `project.recoverable.delete {session}` deletes one. `project.get` and the
   other project-info results count `recoverable_sessions`.
5. **The GUI offers, never loads.** At start without a project argument, and
   from File > Recover unsaved work…, a popup lists the sessions (newest
   first, with their size), each with Recover and Delete (Delete asks once
   more); Later closes it and nothing is loaded. Recover goes through the
   usual unsaved-changes question.

## Consequences

- An untitled project survives a crash up to its last autosave (at most a
  minute of work), and its recordings to the last header rewrite.
- Autosaves read back are checked like any project file, including the rule
  for which saved steps may replay (ADR-0076).
- Every GUI start creates its folder and lock file at once (before, the
  folder appeared with the first recording); quitting or discarding removes
  it as before.
- A folder left by an older build has no lock file; listing it creates one
  and takes it, so an older build still running beside a newer one could
  see its folder offered for recovery.

## Alternatives considered

- **The pid in the folder name decides liveness:** pids are reused, and a
  live process is not proof the folder is still that run's.
- **A clean-exit marker file:** a run that is still running looks like a
  crashed one to a second run; the lock answers both questions.
- **Recovered sessions load automatically:** never; the user decides, as for
  `autosave_available`.
- **A time limit that deletes old sessions:** deleting recordings unasked
  is worse than a list that grows; the popup shows sizes instead.
