# OSTree fetch protocol

## Subject to change

These are considered to be internal OSTree details, so do not write software which depends on these details unless you can keep up with the official implementation.

The file format of static deltas should be currently considered an
OSTree implementation detail.  Obviously, nothing stops one from
writing code which is compatible with OSTree today.  However, we would
like the flexibility to expand and change things, and having multiple
codebases makes that more problematic.  Please contact the authors
with any requests.

That said, one critical thing to understand about the design is that
delta payloads are a bit more like "restricted programs" than they are
raw data.  There's a "compilation" phase which generates output that
the client executes.

This "updates as code" model allows for multiple content generation
strategies.  The design of this was inspired by that of Chromium:
[ChromiumOS Autoupdate](http://dev.chromium.org/chromium-os/chromiumos-design-docs/filesystem-autoupdate).

Deltas can be found in `deltas/$fromprefix/$fromsuffix-$to`.
A delta is itself a directory.  Inside, there is a file called
`superblock` which contains metadata.  The rest of the files will be
integers bearing packs of content.

### The delta superblock

The superblock contains:

 - arbitrary metadata
 - delta generation timestamp
 - the new commit object
 - An array of recursive deltas to apply
 - An array of per-part metadata, including total object sizes (compressed and uncompressed),
 - An array of fallback objects

Let's define a delta part, then return to discuss details:

## A delta part

A delta part is a combination of a raw blob of data, plus a very
restricted bytecode that operates on it.  Say for example two files
happen to share a common section.  It's possible for the delta
compilation to include that section once in the delta data blob, then
generate instructions to write out that blob twice when generating
both objects.

Realistically though, it's very common for most of a delta to just be
"stream of new objects" - if one considers it, it doesn't make sense
to have too much duplication inside operating system content at this
level.

So then, what's more interesting is that OSTree static deltas support
a per-file delta algorithm called
[bsdiff](https://github.com/mendsley/bsdiff) that most notably works
well on executable code.

The current delta compiler scans for files with matching basenames in
each commit that have a similar size, and attempts a bsdiff between
them.  (It would make sense later to have a build system provide a
hint for this - for example, files within a same package).

A generated bsdiff is included in the payload blob, and applying it is
an instruction.

## Fallback objects

It's possible for there to be large-ish files which might be resistant
to bsdiff.  A good example is that it's common for operating systems
to use an "initramfs", which is itself a compressed filesystem.  This
"internal compression" defeats bsdiff analysis.

For these types of objects, the delta superblock contains an array of
"fallback objects".  These objects aren't included in the delta
parts - the client simply fetches them from the underlying `.filez`
object.

# Anatomy of an OSTree repository

## Repository layout

OSTree is deeply inspired by git; the core layer is a userspace
content-addressed versioning filesystem.

If you are familiar with git, the differences can be summarized:
 - OSTree splits "tree" objects into "dirtree" and "dirmeta" objects
 - OSTree's checksums are SHA256
 - Content objects include uid, gid, and extended attributes (but still no timestamps)
 - OSTree supports "bare" and "bare-user" repository formats, but not packfiles
 - There is no index; the API is preferred over the command-line tools
 - OSTree makes it easier to delete data, under the assumption that you can regenerate it from source code

A typical repository is laid out as follows:
```
$ ls /ostree/repo
config # type of repository. See the repo-config manpage.
objects/ # objects, addressed by hash
refs/ # summary file and heads/ tree.
deltas/ # deltas, primarily found on servers
state/ # internal, used for locks
tmp/ # internal, used for downloads
```

## Core object types and data model

The files in the objects directory are content-addressed, by their type and checksum. The subdirectory is named with the first 2 characters of the SHA, and the filename is the remaining characters plus an extension.

### Commit objects

A commit object contains metadata such as a timestamp, a log
message, a parent object, and a list of related objects (undocumented and currently always empty).
But more importantly, it references a dirtree/dirmeta
pair of checksums which describe the root
directory of the filesystem.

### Dirtree objects

A dirtree contains a sorted array of (filename, checksum)
pairs for content objects, and a second sorted array of
(filename, dirtree checksum, dirmeta checksum), which are
subdirectories.

### Dirmeta objects

In git, tree objects contain the metadata such as permissions
for their children.  But OSTree splits this into a separate
object to avoid duplicating extended attribute listings.

### Content objects

Unlike the first three object types which are metadata, designed to be
`mmap()`ed, the content object has a separate internal header and
payload sections.  The header contains uid, gid, mode, and symbolic
link target (for symlinks), as well as extended attributes.  After the
header, for regular files, the content follows.

# Repository types and locations

Also unlike git, an OSTree repository can be in one of three separate
modes: `bare`, `bare-user`, and `archive-z2`.  A bare repository is
one where content files are just stored as regular files; it's
designed to be the source of a "hardlink farm", where each operating
system checkout is merely links into it.  If you want to store files
owned by e.g. root in this mode, you must run OSTree as root.

The `bare-user` is a later addition that is like `bare` in that files
are unpacked, but it can (and should generally) be created as
non-root.  In this mode, extended metadata such as owner uid, gid, and
extended attributes are stored but not actually applied.
The `bare-user` mode is useful for build systems that run as non-root
but want to generate root-owned content, as well as non-root container
systems.

In contrast, the `archive-z2` mode is designed for serving via plain
HTTP.  Like tar files, it can be read/written by non-root users.

On an OSTree-deployed system, the "system repository" is
`/ostree/repo`.  It can be read by any uid, but only written by root.
Unless the `--repo` argument is given to the <command>ostree</command>
command, it will operate on the system repository.

## Refs

Like git, OSTree uses the terminology "references" (abbreviated
"refs") which are text files that name (refer to) to particular
commits.  See the
[Git Documentation](https://git-scm.com/book/en/v2/Git-Internals-Git-References)
for information on how git uses them.  Unlike git though, it doesn't
usually make sense to have a "master" branch.  There is a convention
for references in OSTree that looks like this:
`exampleos/buildmaster/x86_64-runtime` and
`exampleos/buildmaster/x86_64-devel-debug`.  These two refs point to
two different generated filesystem trees.  In this example, the
"runtime" tree contains just enough to run a basic system, and
"devel-debug" contains all of the developer tools and debuginfo.

The `ostree` supports a simple syntax using the caret `^` to refer to
the parent of a given commit.  For example,
`exampleos/buildmaster/x86_64-runtime^` refers to the previous build,
and `exampleos/buildmaster/x86_64-runtime^^` refers to the one before
that.

## The summary file

A later addition to OSTree is the concept of a "summary" file, created
via the `ostree summary -u` command.  This was introduced for a few
reasons.  A primary use case is to be a target a
[Metalink](https://en.wikipedia.org/wiki/Metalink), which requires a
single file with a known checksum as a target.

The summary file primarily contains two mappings:

 - A mapping of the refs and their checksums, equivalent to fetching
   the ref file individually
 - A list of all static deltas, along with their metadata checksums

This currently means that it grows linearly with both items.  On the
other hand, using the summary file, a client can enumerate branches.

Further, the summary file is fetched over e.g. pinned TLS, this
creates a strong end-to-end verification of the commit or static delta.

The summary file can also be GPG signed (detached), and currently this
is the only way provide GPG signatures (transitively) on deltas.

If a repository administrator creates a summary file, they must
thereafter run `ostree summary -u` to update it whenever a commit is
made or a static delta is generated.
