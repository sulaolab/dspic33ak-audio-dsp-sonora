# Documentation policy

The `docs_public/` directory is part of the public source distribution. Files placed
here must be reusable without access to a particular developer workstation or
piece of lab equipment.

Public documentation must not contain:

- local absolute paths or user names;
- programmer/debugger serial numbers;
- raw serial captures or transient benchmark logs;
- private repository names or URLs;
- session handoffs, branch cleanup instructions, or unpublished plans.

Project-internal design notes, research reports, hardware bring-up logs, and
development records belong in the development tree's internal documentation
directory, which is not part of the published snapshot.
Machine-local raw captures and other material that must not be tracked belong
in `notes_private/`; that directory is ignored by Git and must not be force-added.

How a release is produced from the development tree -- the inspection gates and the
archive step -- is a maintainer procedure and is documented with those tools, which are
not part of this distribution. Nothing in it is needed to build or use this source.

This directory's own file list is the current inventory -- avoid restating it
here, since a restated list only goes stale. Internal reports should be promoted out of
the development tree only after they are rewritten as stable, generally reusable
documentation. Where a published file cites one of those records, it is written
as `[internal] <filename>`: the evidence exists, but it is not shipped here. The same
form is used for a record held in an internal sibling project rather than in this
tree -- the point of the marker is that the reader is not sent after a path that
cannot resolve, so a published file must never carry a bare path or repository name
for one.
