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

## Private folder staging

On POSIX, a folder is restored into `contents` inside a private `0700`
staging wrapper. The CLI retains handles to the output parent, wrapper and
archive root. Entry creation, metadata, attributes and rollback use those
handles; publication renames `contents` out of the wrapper. The wrapper's
permissions never change, including while the root and entries receive their
recorded permissions. Replacing the wrapper's pathname with a symlink or
another directory cannot redirect restoration, cleanup or publication.

Forced publication keeps the existing target until rename or exchange succeeds.
POSIX rename atomically replaces empty directories; Linux/macOS exchange can
replace files and symbolic links, retaining the previous entry inside the
wrapper until publication completes. An unavailable exchange refuses safely.
Windows refuses existing folder targets even with `--force`. Cleanup never
recurses into an exchanged entry. If that cleanup or final mode restoration
fails, the published output remains and the previous target is retained in the
wrapper; the error names the wrapper so the caller can recover it.

Temporary entries retain owner access even under umask `0777`. Once their
contents are complete, entries receive their recorded modes; version-1 files
and directories receive `0666 & ~umask` and `0777 & ~umask`, respectively.
Directory modes are deferred until all children are restored.

Moving a read-only root between directories can require owner-write permission.
The CLI temporarily adds only that bit for the move and restores the validated
mode through the retained root handle immediately after publication. If that
final permission operation fails, the command reports that the folder was
published and retains it; it does not recursively delete an exposed output.

Cleanup only traverses the retained archive root. If another process renamed
the wrapper, its empty directory may remain under that new name; replacement
entries at the original staging name are left alone.

On Linux, inherited access and default ACLs are removed from the archive root
before any children are created. Archived ACLs are then restored in the usual
deferred order. An archive without ACLs therefore acquires none from the
destination parent, including when the archive is empty.

## Private file staging

On POSIX, temporary output files live inside a private `0700` wrapper and
stay at mode `0600` during processing and verification. The CLI retains the
parent, wrapper and file handles. Metadata is applied to the held file;
publication and rollback resolve `contents` relative to the held wrapper.
Replacing the wrapper pathname cannot redirect metadata or publication, and
cleanup leaves a replacement entry alone. Linux staging files also
have inherited ACLs removed before final permissions are applied. Successful publication
applies the recorded source mode, or `0666 & ~umask` when no metadata is present.
Failure removes the temporary file. This protects decoded plaintext while
validation is still pending, including when the destination directory is
readable by other users.

## Bounded deferred metadata

Directory metadata and root/directory attributes are written to an
automatically removed temporary journal. A fixed footer links each record
to the previous one, so replay visits children before parents and the root
last, without retaining an index or attribute collection in memory. One
attribute buffer and one bounded path are enough, regardless of entry count.
Replay begins only after the container checksum and archive syntax pass.
Access ACLs are replayed after all other attributes, before final metadata.
Directory replay remembers the ACL's journal offset and reuses the same value
buffer; regular and sparse files retain only their current access ACL (at most
64 KiB) until the metadata callback. A read-only ACL therefore cannot prevent
restoration of later user attributes. A directory handle stays open across
access-ACL and mode changes. Legacy
entries still receive the captured umask-derived modes. Journal creation,
write or replay failure discards staging; temporary disk use grows with
metadata size and the decoded archive remains subject to `--max-output`.

## Verification

The C archive suite checks fragmented version-4 prefixes, truncation, invalid
attributes, callback failures, missing callback support and versions 1–3.
The CLI safety suite checks root xattrs, Linux ACLs, deferred ACL application,
empty and segmented folders, destination ACL inheritance, staging-name
replacement, cleanup after restrictive metadata, and private staging before
the trailer arrives. It also checks restrictive umasks, legacy modes and
cleanup on file-size limits (`SIGXFSZ`). The decoder suite exercises malformed worker results with
null diagnostic pointers.

Source: implemented by OpenAI GPT-6 via Codex using the archive attribute encoding described above.
