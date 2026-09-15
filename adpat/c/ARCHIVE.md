# Archive stream versions and restoration

The outer adapt container carries a byte stream. When its archive flag is
set, that stream describes a directory tree. The archive version is independent
of the predictor/container version: changing archive framing does not change
the predictor or existing container headers.

## Version 4: root attributes

The CLI writes `ADPTARC4` for folders. The decoded stream contains:

1. The eight-byte magic `ADPTARC4`.
2. The root directory's attribute list, including an empty list when appropriate.
3. The entries defined by version 3, with their existing per-entry attributes.
4. The one-byte archive end marker, `0`.

The root list uses the same encoding as an entry's attribute trailer:

| Field | Encoding |
| --- | --- |
| Attribute count | Little-endian unsigned 16-bit integer |
| Name length, per attribute | Little-endian unsigned 16-bit integer; 1–255 |
| Name | Name-length bytes, without a NUL |
| Value length | Little-endian unsigned 32-bit integer; 0–65,536 |
| Value | Value-length bytes |

Names are strictly increasing in byte order; repeated names are invalid.
The root is not a filesystem entry, has no archive path, and does not contribute
to `adapt_archive_entries()`. Its mode and timestamps remain in the outer
container's metadata header. Root extended attributes and Linux POSIX access
and default ACLs now travel with the archive's bytes and checksum.

An SDK reader registers `adapt_archive_set_root_attribute_callback()` before
unpacking version 4. The callback receives the existing sink context, name,
value and length. Missing callback support rejects a nonempty root list instead
of silently losing it. An empty list requires no callback. Nonzero callback
returns stop unpacking as they do for entry callbacks.

A filesystem sink retains root attributes until the container checksum and
archive completion have passed and all children are restored. It then applies
and checks the attributes, followed by the root's recorded mode and timestamps.
This ordering prevents a root default ACL from being inherited by children
whose source entries had different permissions. An attribute restoration
failure discards staging and preserves the existing destination.

## Compatibility

`ADPTARC1`, `ADPTARC2` and `ADPTARC3` retain their original framing and remain
readable. Their writers and public entry-header helpers retain their behavior.
Version-3 entry helpers also write version-4 entries; use
`adapt_archive_attributes()` and `adapt_archive_attribute()` for the new prefix.
Older decoders reject the new archive magic. Root attributes omitted by an
older encoder cannot be recovered from its container.

## Private file staging

On POSIX, temporary output files are created with owner-only access and kept
at mode `0600` during processing and verification. Successful publication
applies the recorded source mode, or `0666 & ~umask` when no metadata is present.
Failure removes the temporary file. This protects decoded plaintext while
validation is still pending, including when the destination directory is
readable by other users.

## Verification

The C archive suite checks fragmented version-4 prefixes, truncation, invalid
attributes, callback failures, missing callback support and versions 1–3.
The CLI safety suite checks root xattrs, Linux ACLs, deferred ACL application,
empty and segmented folders, failure cleanup, and private staging before the
trailer arrives. The decoder suite exercises malformed worker results with
null diagnostic pointers.

Source: implemented by OpenAI GPT-6 via Codex from the existing archive
attribute encoding and the P1 findings reproduced in the 2026-09-15 review.
