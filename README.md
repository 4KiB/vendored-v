On a new `git clone`, go from `cc` to `v *.v`. Call `make` to build `tcc`,
`libgc`, and `v` without any downloads or prebuilt binaries.

The [V](https://github.com/vlang/v) compiler is written in V. It bootstraps by
compiling the (standalone) .c output of an existing `v` binary, with the
philosophy that C is the modern cross-platform assembly language.

Vendoring V also includes vlang's [published translation to C][vc] (which is
updated upstream by an automated process), the [Tiny C Compiler][tinycc], and
the [Boehm-Demers-Weiser garbage collector][libgc].

Vendoring `tcc` source addresses the TinyCC bootstrapping problem as well,
assuming that `cc` is available on the host machine, a safe assumption for a
language with a C/C++ audience, avoiding any method of prebuilt binary that
excludes V's viability on non-prebuilt architectures that are supported by C.

While `v` is usable without `tcc`, V's sensibilities require it for faster
compilation, which is especially important for the REPL and hot reloading.

[vc]: https://github.com/vlang/vc
[tinycc]: https://repo.or.cz/w/tinycc.git
[libgc]: https://github.com/ivmai/bdwgc

With these tools in place, updating vendored V code can be more frequent than
other vendored code, given that V rebuilds itself (with `v self`). It follows
that the self-updating feature `v up` is disabled by a local patch, as
suggested in V's [doc][dist] on packaging `v` for OS distributions.

[dist]: https://github.com/vlang/v/blob/9ad84ddc/doc/

A `.vendor` file in the root of each `src/vendor/` repository contains audit
detail on the download of upstream files. This file is simple enough to update
manually and is simple enough for a small program to manage.\* The format is
the git remote URL at a given reference, followed by one or more lines
modifying the resulting clone.

```
https://github.com/{user}/{repo}.git@0c0ffee

-test
-tests
-*_test

+patch/
```

\* As a result of vendoring, a tool to manage vendor files can be written in V.

Use `#` for comments. The ref is anything understood by `git checkout`. Lines
beginning with `-` are to remove all files and directories that match the given
name, accepting simple `*` wildcards, the equivalent of `find|xargs` where
PATTERN is the line in `.vendor` without the leading `-`:

```
find . -name 'PATTERN' | xargs rm -r
```

Lines beginning with `+` reference a directory relative to the project root:

```
fix.patch
build.sh
path/to/override.txt
another/path/0001.patch
another/path/0002.patch
```

The directory layout mirrors that of the repository. Files ending in `.patch`
apply within the listed directory, otherwise copy over as-is.

Further, it's important to discard .git to (a) keep the resulting repository
from getting too large and (b) avoid confusing `git` by nesting projects. The
git-builtin feature of submodules does not achieve vendoring as it requires an
internet connection and an online remote (with the repo still intact); git
subtree remains relevant as an alternative.

Note that repositories have multiple `.gitignore` at various locations, and
that git can track files that are otherwise ignored; such files need to be
vendored.

The result of vendoring V is a repository that is 60MB, a factor of at least
three less than upstream, having removed git history and project support
files. This compresses down to a 9.7MB git archive, which is how much data is
required on a `git clone`.

In this repository, V application code is at `src/example/`; `v` builds in
place, resulting in `example.v` having its executable at `src/example/example`.

The executable generated from .v code has dependencies typical to dynamically
linked C, namely libc. Call `make static` to use V's `-freestanding` feature to
build a binary that is less than 300kB, and leave the world behind.

This repository is laid out before introducing any of V's concepts of package
management. A project using this template can add `v.mod` as needed.

Adhering to a vendor-everything policy allows for a project to build without
external dependencies, which provides stability in any project but especially
one built with a new programming language. **A very significant (and profound)
advantage is that the programming language implementation becomes
transparent.** Every feature is available for edit and rebuilds instantly. This
allows for both local patches to `v` and patches intended for upstream.

Hacking the language is as natural as hacking the project that builds on it.
