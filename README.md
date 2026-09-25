# Bake
Bake is a build system that provides a simple JSON configuration and fast builds. It provides simplified dependency management by letting projects refer to dependencies using logical names. This decouples projects from what is installed on a system, and prevents having to specify absolute or relative paths to other dependency code.

Bake accomplishes this by creating entries for projects in a bake environment (see below), which allows bake to find projects and build artefacts by logical name.

Bake is not yet-another-cmake-or-make wrapper. Instead, it emits compiler commands directly which speeds up builds and simplifies installation. Bake supports clang, gcc and msvc on MacOS, Linux and Windows.

**WARNING: Heavy work in progress. If you are looking for a stable build system, do not use this!! Take a look instead at https://github.com/SanderMertens/bake**

## Getting started
To install bake, run the following command:

```
./setup.sh

On macOS the setup script ad-hoc signs the binary before installing it; an unsigned replacement of an installed binary is killed by the system (`Killed: 9`). If you copy the binary by hand, run `codesign -f -s - <path>`.
```

This might prompt you for a password once as the script has to to install a script to `/usr/local/bin`. Once this script is installed, providing a password during setup is no longer needed.

## Commands
Build project in current directory:
```
bake
bake .
bake build
bake build .
```

Clean project in current directory:
```
bake clean
```

Rebuild project in current directory:
```
bake rebuild
```

Build project in specified directory:
```
bake my_app
bake build my_app
bake build projects/my_app
```

Clean project in specified directory:
```
bake clean my_app
bake clean projects/my_app
```

Rebuild project in specified directory:
```
bake rebuild my_app
bake rebuild projects/my_app
```

Run project in current directory (will recursively build dependencies):
```
bake run
bake run .
```

Run project in specified directory:
```
bake run my_app
bake run projects/my_app
```

The project is run with its own directory as working directory, so a project can
load files relative to its own location.

Pass arguments to project:
```
bake run -- --key=value
bake run my_app -- --key=value
```

List processes that were started by bake (see
[Orchestrating multiple agents](#orchestrating-multiple-agents)):
```
bake ps
bake ps --full
bake ps --json
bake ps --kill 82190
```

## Usage
```
Usage: bake [options] [command] [target]

Commands:
  build [target]      Build target project and dependencies (default)
  run [target]        Build and run executable target
  test [target]       Build and run test target
  bench [target]      Build and run benchmark target
  clean [target]      Remove build artifacts
  coverage-report [target] Merge test coverage into a browsable report
  rebuild [target]    Clean and build
  list                List projects in bake environment
  ps                  List processes started by bake
  info <target>       Show project info
  cleanup             Remove stale projects from bake environment
  reset               Reset bake environment metadata
  setup               Install bake executable into bake environment
  bundle update [<name>] Update bundle branch checkouts

Options:
  --cfg <mode>        Build mode: sanitize|debug|profile|release
  --cc <compiler>     Override C compiler
  --cxx <compiler>    Override C++ compiler
  --target <name>     Cross-compile target (em = emscripten/wasm)
  --run-prefix <cmd>  Prefix command when running binaries
  --json              ps only: print the process list as json
  --all-users         ps only: include processes of other users in the scan
  --full              ps only: print untruncated workspace and command columns
  --kill <pid|env>    ps only: stop a listed process, environment or env kind
  --build-json <file> Write a json report of the build with per step timings
  --repeat <n>        Build/rebuild only: run the build n times, keep the median report
  --warmup <n>        Build/rebuild only: unmeasured runs before --repeat runs
  --local-env[=<name>] Use ./.bake/local_env (or ./.bake/local_env/<name>) as isolated BAKE_HOME and build root
  --local             Setup only: install into BAKE_HOME (skip /usr/local/bin)
  --standalone        Use amalgamated dependency sources in deps/
  --strict            Enable strict compiler warnings and checks
  --coverage          Build with coverage instrumentation (clang only)
  --coverage-root <dir> Make coverage report paths relative to dir (default: cwd)
  --coverage-include <patterns> Only report files matching a comma separated prefix or glob
  --coverage-exclude <patterns> Leave out files matching a comma separated prefix or glob
  --fix-lint          Run the lint command of projects with the autofix action
  --trace             Enable trace logging (Flecs log level 0)
  -j <count>          Number of parallel jobs for build/test execution
  -r                  Apply command recursively to project and project dependencies
  -h, --help          Show this help
```

## Orchestrating multiple agents
Two features exist for workspaces that several people, agents or CI jobs share at
the same time. Read both before running bake in a shared checkout:

- **[`--local-env[=<name>]`](#named-local-environments)** gives each agent its own
  isolated `BAKE_HOME` and build root inside the workspace, so parallel builds of
  the same checkout do not overwrite each other's artefacts.
- **[`bake ps`](#listing-running-processes)** lists everything bake started (and
  everything started directly from a local environment binary), how long it has
  been running and which environment it came from, so an orchestrator can audit
  and stop what is running.

Typical use:

```
bake run my_app --local-env=agent_a     # build + run in agent_a's environment
bake ps                                 # what is running, where, for how long
bake ps --full                          # same table with untruncated paths
bake ps --kill agent_a                  # stop everything from that environment
bake ps --kill local                    # stop every local environment process
```

### Listing running processes
`bake ps` prints one row per running process that bake knows about:

```
$ bake ps
PID    ELAPSED  ENV            CFG    PROJECT      STATE    SOURCE    WORKSPACE           CMD
82190  0:04:12  local:agent_a  debug  night_shift  running  registry  ~/dev/flecs-engine  /Users/me/dev/flecs-engine/.bake/local_env/agent_a/a...
82355  0:00:47  local:agent_b  debug  night_shift  running  scan      ~/dev/flecs-engine  /Users/me/dev/flecs-engine/.bake/local_env/agent_b/a...
82420  0:00:12  local          debug  night_shift  running  registry  ~/dev/flecs-engine  /Users/me/dev/flecs-engine/.bake/local_env/arm64-Da...
82512  0:00:05  global         debug  my_app       running  registry  ~/dev/my_app        /Users/me/bake3/arm64-Darwin/debug/bin/my_app
run 'bake3 ps --full' to see full paths
```

Columns:

- `PID`: process id of the started process (not of bake itself)
- `ELAPSED`: how long the process has been running, as `h:mm:ss`
- `ENV`: which environment the process came from: `local:<name>` for
  `--local-env=<name>`, `local` for `--local-env` without a name, `global` for the
  global `BAKE_HOME`, and `?` when the environment cannot be determined
- `CFG`: build mode the binary was built with
- `PROJECT`: project id
- `STATE`: `running`, `zombie`, or `orphan` when the bake process that started it is gone
- `SOURCE`: `registry` or `scan` (see below)
- `WORKSPACE`: directory bake was invoked from, shortened for display
- `CMD`: command line, truncated

`WORKSPACE` and `CMD` are shortened to keep the table readable, and a line under
the table points at `bake ps --full`, which prints both columns in full. That line
is never printed with `--full` or `--json`, and it comes after the rows so the
table stays parsable.

Processes are found in two ways, and both end up in the same table:

- `registry`: every time `bake run` or `bake test` starts a process, bake writes
  `<pid>.json` with the project, cfg, environment, `BAKE_HOME`, workspace, command
  line, start time and the pid of the bake process that started it. The file is
  removed when the process exits, and entries whose process is gone are pruned the
  next time `bake ps` runs. The registry lives in `~/.bake3/ps` so it spans every
  workspace on the machine; set `BAKE3_PS_DIR` to point it somewhere else (tests do
  this). It is per user: `bake ps` never reads another user's registry.
- `scan`: bake also scans the process table for running executables whose path is
  inside a `.bake/local_env` directory. This finds processes that were started
  directly from a local environment binary instead of through `bake run`, for
  example a tool that execs
  `.bake/local_env/agent_a/arm64-Darwin/debug/bin/my_app` itself. The environment
  name, workspace, config and project are read from that path. The scan is POSIX
  only; on Windows `bake ps` lists registry entries and says so.

Options:

- `--json`: print the same information as a json array, with `elapsed_sec`,
  `start_time`, `parent_pid` and `bake_home` included. `env` holds the same label
  the `ENV` column shows, with `env_kind` (`local`, `global` or `unknown`) and
  `env_name` (the local environment name, `-` when there is none) next to it. Use
  this from scripts.
- `--all-users`: include processes owned by other users in the process table scan.
- `--full`: print the `WORKSPACE` and `CMD` columns in full instead of shortening
  them, and drop the hint line under the table.
- `--kill <pid|env>`: send a terminate signal to a listed process (by pid), to
  every listed process of a local environment (by environment name, or by its
  `local:<name>` label), or to every listed process of an environment kind
  (`local` or `global`), wait for them to stop and force kill what is left. Only
  processes that `bake ps` lists can be killed this way; an unknown pid is an
  error, never a signal to an unrelated process. A kind is a wide target: `local`
  matches every listed local environment process, in every workspace the scan
  reached, so prefer a pid or an environment name to stop one agent's work.

Not everything can be detected: a process started directly from a binary that is
not inside `.bake/local_env` (a global environment build, or a copy of the binary),
a process whose path contains spaces, and processes started through a wrapper
script that replaces `argv[0]` are invisible to the scan. Those show up only when
they were started with `bake run` or `bake test`.

When `bake run` starts a process it prints a single line naming the pid and
environment, as a reminder that `bake ps` exists:

```
[bake] started my_app (pid 82190, env local:agent_a) - run 'bake3 ps' to see running processes
```

## Project structure
Bake projects store files in well known locations, to keep build configuration simple. Those locations are:

- `src`: Directory that stores the source files. May contain other directories that store source files.
- `include`: Directory that stores public include files. Files in this directory will be accessible to the project as well as dependees.
- `etc`: Project assets. These will be accessible from the project binary when the project is ran.

Every build of a public project installs its `etc` folder into the bake
environment, at `$BAKE_HOME/etc/<project id>`. The install mirrors the project:
changed files are refreshed, files that no longer exist in `<project>/etc` are
removed from the installed copy, and other projects' folders are never touched.
Projects with `"public": false`, and projects without an `etc` folder, install
nothing. A process started with `bake run` finds the folder through `BAKE_HOME`
(see [Bake Environment](#bake-environment)), which lets an installed package
ship assets that are found no matter which directory the binary runs from. The
install path of a project is printed by `bake info <project>`.

When a project is built, the build artefacts will be stored in:

- `.bake/arch-os-config`: Stores the executable or library binary
- `.bake/arch-os-config/obj`: Stores the generated object files
- `.bake/arch-os-config/generated`: Stores generated files (such as header dependencies)

For example:

`.bake/arm64-Darwin-debug/libflecs.a`

When `--local-env` is used, build artefacts are isolated to:

- `.bake/local_env/build/<project-id>/arch-os-config`: Stores the executable or library binary
- `.bake/local_env/build/<project-id>/arch-os-config/obj`: Stores generated object files
- `.bake/local_env/build/<project-id>/arch-os-config/generated`: Stores generated files (such as header dependencies)

When `--local-env=<name>` is used, build artefacts are isolated to:

- `.bake/local_env/<name>/build/<project-id>/arch-os-config`: Stores the executable or library binary
- `.bake/local_env/<name>/build/<project-id>/arch-os-config/obj`: Stores generated object files
- `.bake/local_env/<name>/build/<project-id>/arch-os-config/generated`: Stores generated files (such as header dependencies)

## Project configuration
Bake projects are configured with a `project.json` file in the root of the project directory. The simplest configuration looks like this:

```json
{
    "id": "my_app",
    "type": "application"
}
```

The type of a project may be either `application` or `package`, which respectively builds an executable or library.

Project settings can be configured in a `value` property. For example:

```json
{
    "id": "my_app",
    "type": "application",
    "value": {
        "use": ["flecs", "cglm"]
    }
}
```

The following options are supported:
- `use`: Specify a list of dependencies. Dependencies always use logical names which match exactly the name provided in the `id` field.
- `use-private`: Same as `use`, but headers of dependencies will not be visible to dependees.
- `language`: Specify the language. May be `c` (default), `c++` or `cpp` (same as `c++`).
- `public`: When false, the project will not be copied to the bake environment (see below). Default is true.
- `amalgamate`: Specify whether the project should amalgamated the source files.
- `amalgamate-path`: Destination path for the output of the amalgamation process.
- `output`: Name of the build artefact. Defaults to the project id.
- `standalone`: When true, this will copy all amalgamated sources from dependencies to a `deps` folder in the project, and include those in the project build rather than relying on linking with dependency binaries. This allows for the project to be easily shared, without having to also share the dependencies. The sources in `deps` are refreshed automatically when a dependency changes. When the dependency sources are not available (for example on a machine that only has the standalone project), the existing sources in `deps` are used as is.

## Linting
A project can run a linter on every source file bake compiles by adding a
`lint` section to its configuration:

```json
{
    "id": "my_app",
    "type": "application",
    "lint": {
        "command": "python3 tools/lint.py",
        "action": "error"
    }
}
```

Before compiling a file, bake runs `<command> <file> <action>` from the project
directory, where `<file>` is the absolute path of the source file and `<action>`
is one of:

- `error` (default): a non-zero exit code of the lint command fails the build.
- `log`: a non-zero exit code is reported as a warning and the build continues.
- `autofix`: the lint command may rewrite the file before it is compiled. A
  non-zero exit code is reported as a warning and the build continues.

The lint command runs for the files under `src` and `test` that bake compiles,
so files that are up to date are not linted again. Sources in `deps`, bundle
sources and the generated `main.c` of test and benchmark projects are not
linted. A file that fails the lint is not compiled, so the next build lints it
again.

Everything the lint command writes to stdout and stderr is printed after it
exits, one line at a time with a `lint: ` prefix. The prefix is red when the
file fails the build and yellow otherwise, and a leading `file:line:` location
is printed in bold.

`--fix-lint` runs the lint command with the `autofix` action, whatever action
the projects configure. Like the lint itself it only covers the files bake
compiles, so use `bake rebuild --fix-lint` to fix every file of a project.

## Language configuration
Projects can configure options that are specific to the programming language of the project by adding a `lang.c` or `lang.cpp` section to the project configuration. For example:

```json
{
    "id": "my_app",
    "type": "application",
    "value": {
        "use": ["flecs", "cglm"]
    },
    "lang.c": {
        "lib": ["m", "pthreads"]
    }
}
```

The following configuration options are available:

- `cflags`: list of arguments to pass to the C compiler
- `cxxflags`: list of arguments to pass to the C++ compiler
- `ldflags`: list of preprocessor defines to add to the linker
- `lib`: list libraries to link with
- `libpath`: list of paths to use for resolving  libraries
- `defines`: list of preprocessor defines to add to the compiler
- `include`: list of additional include paths
- `embed`: list of files and directories to embed in the artefact (Emscripten only, see below)
- `shell`: html page emcc uses as the template for the artefact (Emscripten only, see below)
- `c-standard`: Specify the C standard to use for C files
- `cpp-standard`: Specify the C++ standard to use for C++ files

Each configuration key has a single supported spelling. When bake finds an
unknown key that resembles a supported one, it says so instead of ignoring it:

```
[warning] unknown project key 'libs', did you mean 'lib'?
```

Keys that resemble nothing bake knows (`author`, `description`) stay silent, so
project metadata does not produce warnings.

## Dependee configuration
Projects may add a `dependee` section to their project configuration which contains configuration that will be applied to dependee projects. The structure of a dependee object mirrors that of the project configuration. The following example makes sure that any project that uses `my_library` will also have `flecs` as a dependency and link with `libm`.

```json
{
    "id": "my_libary",
    "type": "package",
    "value": {
        "use": ["flecs", "cglm"]
    },
    "dependee": {
        "value": {
            "use": ["flecs"]
        },
        "lang.c": {
            "lib": ["m"]
        }
    }
}
```

When a dependee configuration specifies a property that accepts a list, the list will be appended to the dependees own list if it existts. For example:

```json
{
    "id": "my_app",
    "type": "package",
    "value": {
        "use": ["my_library", "cglm"]
    }
}
```

This project will have `my_library`, `cglm` and `flecs` as dependencies (in addition to linking with `libm`).

## Bundles
A project can declare external dependencies that bake fetches and builds automatically. These are useful for pulling in third-party libraries that are not bake projects themselves. Each entry under `bundle` maps a logical name (which can be referenced from `use`) to a git repository. Bake clones the repository, runs CMake to configure, build and install it, and registers the result as a dependency.

```json
{
    "id": "my_app",
    "type": "application",
    "value": {
        "use": ["glfw"]
    },
    "bundle": {
        "glfw": {
            "repository": "https://github.com/glfw/glfw.git",
            "tag": "3.4",
            "library": "glfw3",
            "cmake-args": ["-DGLFW_BUILD_DOCS=OFF", "-DGLFW_BUILD_TESTS=OFF"]
        }
    }
}
```

The following options are supported per bundle entry:
- `repository`: Git URL or local path to clone (required).
- `branch` / `tag`: Optional ref to check out at clone time.
- `commit`: Optional specific commit hash to check out (will fetch full history if needed).
- `subdir`: Optional path inside the cloned repository where the build manifest lives.
- `library`: Name of the library produced (without `lib` prefix or extension). Defaults to the bundle id.
- `build-system`: `"cmake"` (default) or `"cargo"`. With `cargo`, bake runs `cargo build --release --target-dir <build>` instead of cmake (`--release` is dropped only for `--cfg sanitize`, so debug builds still link an optimized crate); cross-compilation targets are forwarded to Cargo and the artefact is resolved from the corresponding target directory.
- `header-only`: When `true`, skip the build step entirely and only expose the cloned source tree as include paths (useful for header-only libraries).
- `include`: List of subdirectories of the bundle source to add to the consuming project's include path (in addition to the default `<install>/include` for built bundles or the bundle root for header-only bundles).
- `sources`: List of source files (relative to the bundle source) to compile alongside the consuming project's own sources. Useful for "drop-in" `.c` files like miniz.
- `cmake-args`: List of extra arguments passed to `cmake` during configuration.
- `cargo-args`: List of extra arguments passed to `cargo` for a `cargo` bundle, appended after the manifest and target directory. Each entry is passed as a single argument, so options that take a value must use the `--opt=value` form (for example `"--no-default-features"` and `"--features=raster-images"`).
- `lib`: System libraries the bundle depends on at link time.
- `ldflags`: Extra link flags to apply when linking the consuming project.

Bundles are project-scoped: each project's bundles are fetched and built under that project's own `.bake/bundles/<id>/<ref>/{src,build/<triplet>,install/<triplet>}` tree, where `<ref>` is `commits/<hash>`, `tags/<tag>`, `branches/<branch>`, or `default` (when no ref is pinned). The most specific ref wins (`commit` > `tag` > `branch`). Two projects in the same workspace pinning different versions of the same bundle do not interfere with each other. Bundles are fetched once per ref-scoped path; subsequent builds reuse the checkout until it is explicitly updated.

Use `bundle update` from a project or workspace directory to fetch and fast-forward every mutable bundle checkout found there, or name one bundle to update only that checkout:

```sh
bake3 bundle update
bake3 bundle update glfw
```

An update uses the configured repository and branch. For a bundle without an explicit branch, it updates the branch currently checked out. Commit- and tag-pinned bundles are left unchanged. The command refuses to update a checkout with tracked or untracked modifications, a detached or unexpected branch, or history that cannot be fast-forwarded. A bundle must have been fetched by a build before it can be updated.

Builds do not contact network remotes to look for changes. When a bundle's configured repository is a local path on disk, bake compares that repository's branch with the checkout. If the checkout is behind, the build continues with the existing revision and prints:

```
[warning] bundle 'glfw' checkout is behind its remote branch 'main'; run bake3 bundle update glfw
```

Bundles live outside of the bake environment, which means they are shared between local environments (see `--local-env`): a bundle is cloned and built once per project and ref, and every named environment of that project links against the same install tree. Concurrent bakes are safe: bake takes a lock (`.bake/bundles/<id>/<ref>/.lock`) around fetching and building a bundle, so a second bake waits for the first one to finish instead of cloning or building over it. A lock whose owning process is gone, or that is older than two hours, is treated as stale and removed.

What a bundle contributes does propagate transitively: the include paths, library paths, libraries and `ldflags` a bundle adds to its own project are also applied to every project that uses it, so a dependee links against the bundle without redeclaring it. The exception is `sources`, which are compiled into the declaring project only.

## Conditional configuration
Sometimes a project may want to apply a configuration only on a specific operating system or for a specific build target. This can be accomplished by surrounding the conditional configuration like so:

```json
{
    "id": "my_app",
    "type": "application",
    "value": {
        "use": ["flecs", "cglm"]
    },
    "lang.c": {
        "${os linux}": {
            "lib": ["m", "pthreads"]
        }
    }
}
```

The following conditional kinds are supported:
- `${os <name>}`: matches the build target OS (`Darwin`, `Linux`, `Windows`, `Emscripten`, ...). This equals the host OS unless cross-compiling with `--target`.
- `${arch <name>}`: matches the build target architecture (`arm64`, `x64`, `wasm32`, ...).
- `${cfg <mode>}`: matches the build mode (`debug`, `release`, ...).
- `${target <name>}`: matches the `--target` value (e.g. `em`).

Conditional blocks may appear at the top level (where they can introduce e.g. `bundle` entries) as well as inside `lang.c`, `lang.cpp` and `dependee` sections.

## Cross-compilation
Bake can cross-compile a project to a non-host target with `--target <name>`. The only target currently supported is `em` (Emscripten / WebAssembly):

```sh
bake build --target em
bake build my_app --target em
```

When `--target em` is used, bake:
- defaults the compiler to `emcc` / `em++` (still overridable with `--cc` / `--cxx`),
- archives static libraries with `emar`,
- configures `bundle` dependencies with `emcmake cmake`,
- builds Cargo bundles with `--target wasm32-unknown-emscripten` and `--crate-type staticlib`,
- emits a `wasm32-Emscripten-<cfg>` triplet so wasm artefacts never clash with native ones,
- gives application targets a `.js` artefact, or an `.html` one when a `shell` is configured (emscripten also emits the sibling `.wasm` file next to it),
- passes the configuration's optimisation level to the link as well as the compile, because for emcc the link-time `-O` level selects which variant of the system libraries is linked,
- omits `-pg` from `--cfg profile`, which emcc does not implement.

### Finding the SDK
If `emcc` is not already on `PATH`, bake sources `emsdk_env.sh` from the first of `$EMSDK`, `$EMSDK_DIR` and `~/GitHub/emsdk` that has one, and imports the resulting `PATH` and `EM*` variables for the build. Set `EMSDK` to use a specific installation:

```sh
EMSDK=~/GitHub/emsdk-latest bake build my_app --target em
```

### Target-specific flags
Use `${os Emscripten}` / `${target em}` conditionals in `project.json` to select target-specific dependencies and flags:

```json
"lang.c": {
    "${target em}": {
        "cflags": ["--use-port=emdawnwebgpu"],
        "ldflags": ["-sUSE_GLFW=3", "--use-port=emdawnwebgpu", "-sDEFAULT_TO_CXX", "-Wl,-u,ntohs"],
        "embed": ["etc"],
        "shell": "etc/index.html"
    }
}
```

Bake adds `-s ALLOW_MEMORY_GROWTH=1`, `-s EXPORTED_RUNTIME_METHODS=cwrap`, `-s MODULARIZE=1` and `-s EXPORT_NAME="<output>"` to the link. These are emitted *before* the project's own `ldflags`, so a project that wants different values simply sets them (`"ldflags": ["-sMODULARIZE=0"]`). With `MODULARIZE=1` the `.js` file defines a factory function named after the project output rather than starting the module, so the page has to call it.

### Embedding assets
`embed` entries become `--embed-file` arguments, which place the bytes in the wasm data segment. Relative entries are resolved against the project directory, and are mounted in MEMFS under the path as written, so the same relative path works on both targets:

```json
"lang.c": {
    "${target em}": { "embed": ["etc"] }
}
```

`<project>/etc` is embedded and `fopen("etc/config.json")` finds it, because emscripten's working directory is `/` and bake runs native builds from the project directory. Two escape hatches are passed through untouched: an absolute path, and an explicit `src@dst` mapping that names the MEMFS destination.

### The page
`shell` names an html file, relative to the project directory, that emcc uses as the template for the artefact (`--shell-file`). Setting it makes the artefact an `.html` file instead of a `.js` one; the `.js` and `.wasm` are still written next to it. The template must contain the `{{{ SCRIPT }}}` placeholder, which emcc replaces with a `<script>` tag for the `.js` file.

`bake run --target em` cannot execute a wasm artefact, so instead it serves the directory the artefact was linked into over a local http server and opens the artefact (or an `index.html` next to it) in a browser. Set `BROWSER` to a command that does nothing (`BROWSER=true`) to keep it from opening a window.

The server listens on port 8080 by default. `--port <n>` picks a different one:

```sh
bake run my_app --target em --port 9000
```

If the port is taken the next free one is used, scanning up to 32 ports past the one asked for; the url the server settles on is printed when it starts. The equivalent by hand is:

```sh
python3 -m http.server 8080 -d <project>/.bake/wasm32-Emscripten-<cfg>
```

Syncing an emscripten artefact into the bake environment copies the sibling `.wasm`, `.data` and `.js` files along with it, so an installed artefact is loadable.

## Test projects
A project with a `test` section in its `project.json` is a test project. Each
listed test case is a `void <Suite>_<case>(void)` function in `src/<Suite>.c`;
bake generates stubs for cases that are missing, together with the harness
`main`. Every case runs in its own process, so a crash only takes down that case.

```json
{
    "id": "core",
    "type": "application",
    "value": { "use": ["flecs"] },
    "test": {
        "testsuites": [{
            "id": "Entity",
            "testcases": ["new", "delete"]
        }]
    }
}
```

A case that runs longer than its timeout is killed, together with the processes
it spawned, and reported as a timeout. The default is 60 seconds per case; a
suite sets its own with a `"timeout"` field (in seconds) next to its
`testcases`:

```json
{
    "id": "Entity",
    "testcases": ["new", "delete"],
    "timeout": 300
}
```

The test binary accepts these arguments after `--`:

- `Suite` runs a single suite, `Suite.case` runs a single case in-process.
- `-j <count>` runs cases in parallel.
- `--timeout <seconds>` overrides the timeout of every case, whatever the suites
  declare. `--timeout 0` disables timeouts. A case that is killed counts as a
  failure, so the run exits non-zero.
- `--json <path>` writes a report with the outcome and wall-clock time of every case:

```sh
bake3 run test/core --local-env -- -j 12 --json /tmp/core.json
```

```json
{
  "project": "core",
  "timestamp": "2026-09-17T20:15:03Z",
  "pass": 2, "fail": 1, "empty": 0, "timeout": 1,
  "elapsed": 60.412,
  "tests": [
    {"suite": "Entity", "case": "new", "status": "pass", "elapsed": 0.004},
    {"suite": "Entity", "case": "delete", "status": "pass", "elapsed": 0.003},
    {"suite": "Entity", "case": "hang", "status": "timeout", "elapsed": 60.001}
  ]
}
```

A case's `status` is `pass`, `fail`, `timeout` (killed after exceeding its
timeout), `empty` (no test statements), `quarantined` or `error` (the case
command could not be built). Parameterized runs add a `params` field.

`fail` counts every failed case, including the timed out ones; `timeout` counts
how many of those were killed by the timeout.

### Coverage
`--coverage` builds every project with clang source-based coverage
instrumentation (`-fprofile-instr-generate -fcoverage-mapping`). It is accepted
by `build`, `rebuild`, `run`, `test` and `bench`, and requires clang: bake
fails with an error for other compilers (use `--cc clang --cxx clang++`) and on
Windows and emscripten. The flags are part of the build fingerprint, so turning
`--coverage` on or off rebuilds the affected projects.

When `bake run` or `bake test` runs a test project built with `--coverage`
with `--json <path>`, bake also writes a coverage report next to the test
report once the harness has written it. The `.json` extension of the path is
replaced by `.coverage.json`:

```sh
bake3 run test/core --local-env --coverage -- -j 12 --json /tmp/core.json
# writes /tmp/core.json and /tmp/core.coverage.json
```

```json
{
  "project": "core",
  "timestamp": "2026-09-25T16:22:29Z",
  "lines": {"count": 16, "covered": 8, "percent": 50.00},
  "functions": {"count": 3, "covered": 1, "percent": 33.33},
  "branches": {"count": 4, "covered": 3, "percent": 75.00},
  "files": [
    {"file": "lib/src/lib.c",
     "lines": {"count": 16, "covered": 8, "percent": 50.00},
     "functions": {"count": 3, "covered": 1, "percent": 33.33},
     "branches": {"count": 4, "covered": 3, "percent": 75.00},
     "uncovered_lines": [[9, 9], [11, 17]],
     "uncovered_functions": [{"name": "lib_unused", "line": 14}]}
  ]
}
```

- `lines`, `functions` and `branches` hold totals over all files, and the same
  counts per file. `percent` is `100.00` when there is nothing to count.
- `uncovered_lines` lists the lines that never ran as inclusive `[first, last]`
  ranges. A range spans lines without code (blank lines, comments), so it
  describes a block that did not run.
- `uncovered_functions` lists the functions that were never called, with the
  line they start on.
- Sources of the test project itself and generated harness sources are left
  out, so the report covers the code under test.
- `files` is sorted by path.

#### Paths and filters
`file` is the path of the source relative to the coverage root, which is the
directory bake runs in unless `--coverage-root <dir>` sets another one (a
relative `<dir>` is resolved against the current directory). Sources outside
the root keep their absolute path.

`--coverage-include <patterns>` keeps only the files that match one of a comma
separated list of patterns, and `--coverage-exclude <patterns>` leaves out the
files that match one. A file is matched on its path relative to the root (its
absolute path when it is outside the root):

- A pattern without `*` or `?` is a path prefix that matches whole path
  components: `src` matches `src/lib.c` and `src/sub/lib.c`, not `srcx/lib.c`.
- A pattern with `*` or `?` is a glob that has to match the whole path. `*`
  matches any characters except `/`, `**` also matches `/` (so `src/**/*.c`
  matches `src/lib.c` and `src/sub/lib.c`) and `?` matches one character
  except `/`.

The totals are the sums over the files that are kept:

```sh
bake3 run test/core --local-env --coverage --coverage-include src,include \
  --coverage-exclude 'src/addons/**' -- -j 12 --json /tmp/core.json
```

The options are accepted by `run` and `test` together with `--coverage`, and by
`coverage-report`, which applies them the same way to `coverage.json`, the
summary and the html report.

Every case process writes its profile to `coverage/` in the build directory of
the test project (`.bake/<arch-os-config>/coverage`, or
`.bake/local_env/<name>/build/<project>/<arch-os-config>/coverage` with
`--local-env`). A run of all cases or of a suite starts by removing the
profiles of the previous run. After the run bake merges the profiles
into `coverage.profdata` with `llvm-profdata`, exports them to
`coverage.lcov` with `llvm-cov` and converts that into the report. Both tools
are looked up with `<cc> -print-prog-name`, so they match the compiler that
instrumented the code. The merged files stay in the coverage directory for
other tools, such as `llvm-cov show`. A case that is killed by its timeout
does not write a profile.

`bake run` and `bake bench` of a project built with `--coverage` also send
profiles to that directory, so instrumented binaries do not leave
`default.profraw` files in the project. `bake clean` removes the coverage
directory together with the rest of the build output, and a `default.profraw`
in the project directory.

### Coverage reports
`bake coverage-report [target]` merges the coverage of every test project in
the target (the current directory by default) into one report. Unlike
`build`, it looks inside `test` and `tests` directories, so running it in the
root of a repository picks up all of its test projects. A target that is not a
directory selects a single test project by id. The report uses the profiles
the last test run of each project left behind, so run the tests with
`--coverage` first, using the same `--local-env`, `--cfg` and `--cc` as the
report:

```sh
bake3 run test/core --local-env --coverage -- -j 12
bake3 run test/query --local-env --coverage -- -j 12
bake3 coverage-report --local-env
```

The command prints a summary with the line, function and branch coverage of
every directory and the total, and writes the report to
`.bake/local_env/coverage_report` (`.bake/local_env/<name>/coverage_report`
with `--local-env=<name>`, `.bake/coverage_report` in the current directory
without `--local-env`):

- `index.html`: a page for browsing the report. It opens from disk, without a
  web server. The overview has the totals and sortable tables of directories
  and files. The sidebar lists every file as a tree, which can be filtered,
  limited to files with uncovered lines, and sorted by coverage. A file shows
  its source with the execution count of every line: covered lines are green,
  lines that never ran are red, and lines with a branch that was never taken
  are yellow, with the number of taken branches next to them. `n` and `p` jump
  between uncovered blocks, functions that were never called link to their
  line, and every line has a link that can be shared.
- `files/<n>.js`: source and line data of one file, loaded when the file is
  opened.
- `coverage.json`: the report in the format of the test harness report, with a
  `projects` array that lists the test projects it combines.
- `coverage.profdata` and `coverage.lcov`: the merged profile and its lcov
  export, for use with other tools.

When test projects share code, such as a library they all link, the counts of
that code are added up, so a line is covered when any of the test projects ran
it. Sources of the test projects themselves and files without code are left
out. `bake clean` removes the report.

## Benchmarks
A project with a `bench` section in its `project.json` is a benchmark project.
It mirrors [test projects](#test-projects): each listed benchcase is a
`void <Suite>_<case>(bench_t *b)` function in `src/<Suite>.c`, bake generates
stubs for cases that are missing together with the harness `main`, and
`bake3 run bench/<name>` (or `bake3 bench <name>`) builds and runs it. Cases run
sequentially, because parallel cases would disturb each other's measurements,
and every case runs in its own process, so a case that crashes or hangs only
takes down that case and the run continues with the next one.

```json
{
    "id": "core",
    "type": "application",
    "value": { "use": ["flecs"] },
    "bench": {
        "benchsuites": [{
            "id": "Entity",
            "benchcases": ["new", "add_component"],
            "setup": true,
            "teardown": true
        }]
    }
}
```

`setup` and `teardown` are optional and generate `void <Suite>_setup(void)` /
`void <Suite>_teardown(void)`, which run once around each benchcase (not around
each sample). A project declares either `test` or `bench`, not both.

### Writing a benchcase
The framework owns the iteration count. A case loops until `bench_iter` returns
false; everything inside the loop is measured:

```c
#include <bake_bench.h>

void Entity_new(bench_t *b) {
    ecs_world_t *world = ecs_init();

    bench_set_items(b, 1);

    while (bench_iter(b)) {
        ecs_entity_t e = ecs_new(world);
        bench_keep(e);
        bench_counter(b, "entities", 1.0);
    }

    ecs_fini(world);
}
```

The API lives in `bake_bench.h`, which bake copies into the project's generated
directory (the same way `bake_test.h` is copied for tests):

- `bool bench_iter(bench_t *b)`: hot loop condition. Inline; it warms up, scales
  the iteration count and closes samples without leaving the loop.
- `void bench_pause(bench_t *b)` / `void bench_resume(bench_t *b)`: exclude
  per-iteration setup from the measured time. Time between a pause and a resume
  is subtracted from the sample.
- `bench_keep(value)`: barrier that keeps a value from being optimized away
  (`asm volatile` with a memory clobber on gcc/clang, `_ReadWriteBarrier` plus a
  volatile sink elsewhere, where the value must be an addressable lvalue).
  `bench_do_not_optimize(value)` is an alias.
- `bench_clobber()`: memory barrier that forces pending writes to be committed.
- `void bench_counter(bench_t *b, const char *name, double value)`: user counter.
  Values are summed over measured iterations and reported as a total and a
  per-iteration average. Calls during warmup are ignored. Up to 16 counters.
- `void bench_set_items(bench_t *b, int64_t items)`: items processed per
  iteration, which adds an items/second figure to the line and the report.
- `uint64_t bench_iterations(const bench_t *b)`: measured iterations so far.

### How a case is measured
Every timestamp comes from a monotonic clock (`CLOCK_MONOTONIC`,
`QueryPerformanceCounter` on Windows).

1. **Warm up**: run a round, double (up to 100x) the iteration count until a
   round takes at least the sample target, then keep the size until two rounds
   agree within 10%. Warmup is capped by its own budget (a quarter of the case
   budget, at most 100 ms) and by 30 rounds.
2. **Scale**: iterations per sample = sample target / the warmup estimate, so a
   sample takes at least the target time (default 10 ms).
3. **Sample**: collect samples of that size (default 100) until the sample count
   or the per-case time budget (default 1 s) is reached. At least one sample is
   always collected.

Each sample contributes one number: nanoseconds per iteration. Over those
samples bake reports mean, median, standard deviation, min, max, p95 and p99
(linear interpolation), and a 95% confidence interval **for the mean**, computed
as `mean ± 1.96 * stddev / sqrt(n)` from the sample standard deviation (a normal
approximation, not a bootstrap). Outliers are counted with Tukey fences on the
samples: `outliers` counts samples outside 1.5 IQR, `outliers_severe` outside
3 IQR. One line is printed per case:

```
Entity.new                             25.310 ns/iter  ci95 [25.180, 25.440]  iters 397000  samples 100  outliers 4
```

`iters` is the iteration count of one sample; the report also carries the total.

### Running benchmarks
```sh
bake3 run bench/core --local-env
bake3 bench bench/core -- Entity.new --time 2 --samples 50
```

Arguments after `--` go to the benchmark binary:

- `Suite` runs one suite, `Suite.case` runs one case.
- `--filter <substr>` runs the cases whose `Suite.case` name contains `substr`.
- `--time <sec>`: wall-clock budget per case (default 1).
- `--samples <n>`: samples to collect per case (default 100).
- `--sample-time <sec>`: target duration of one sample (default 0.01).
- `--json <path>`: write the report below.
- `--baseline <report.json>`: compare medians against an earlier report.
- `--threshold <frac>`: relative change that counts as a regression or an
  improvement (default 0.05).
- `--timeout <sec>`: wall-clock limit for a single case, including its `setup`
  and `teardown` (default 600, `0` disables it). A case that exceeds it is
  killed and prints `TIMEOUT <Suite>.<case>`; the run continues with the next
  case and exits with a non-zero code.
- `--in-process`: run every case in the harness process instead of in a process
  of its own, for example to attach a debugger or profiler to the whole run. A
  crash then ends the run, and so does a timeout.
- `--fail-on-regression`: exit non-zero when a case regressed beyond the
  threshold. Without it a regression is reported but the exit code stays 0.
- `--list-benches`, `--list-suites`: print what the binary contains.

### Crashes and timeouts
The harness starts one process per case: it runs the benchmark binary again
with the name of the case and an internal `--bench-child <file>` argument. That
process measures the case and writes its samples to the file; the harness reads
them back, computes the statistics and prints the line for the case. What a
case prints itself appears above its line.

A case that does not produce a result is reported on its own line and the run
goes on:

```
Alpha.add                              0.312 ns/iter  ci95 [0.311, 0.313]  iters 32051282  samples 100  outliers 3
CRASH Alpha.boom (signal 11: Segmentation fault: 11)
TIMEOUT Alpha.hang (exceeded 600 seconds)
ERROR Alpha.empty (exit code 1)
Alpha.mul                              0.624 ns/iter  ci95 [0.622, 0.626]  iters 16025641  samples 100  outliers 1
-----------------------------
core: 2 benchmark(s) in 603.113s
3 benchmark(s) failed:
FAILED Alpha.boom (crash)
FAILED Alpha.hang (timeout)
FAILED Alpha.empty (error)
```

- `CRASH`: the process was ended by a signal (an exception code on Windows).
- `TIMEOUT`: the case exceeded `--timeout` and was killed.
- `ERROR`: the process exited with a non-zero code or without a result, for
  example because the case never called `bench_iter`.

Any failed case makes the run exit with a non-zero code.

A baseline comparison adds the relative change to each line and lists the
regressions at the end:

```
Entity.new                             31.100 ns/iter  ci95 [30.900, 31.400]  iters 320000  samples 100  outliers 2  +22.9% vs baseline (regression)
-----------------------------
core: 1 benchmark(s) in 1.204s
baseline old.json: 1 regression(s), 0 improvement(s) beyond 5.0%
REGRESSION Entity.new +22.9%
```

### Report format
`--json` writes the summary statistics of every case in nanoseconds, in the
same field style as the test report. Individual samples are not stored:

```json
{
  "project": "core",
  "tool": "bake3",
  "tool_version": "1.0.0",
  "timestamp": "2026-09-18T07:50:34Z",
  "host": {"os": "Darwin", "arch": "arm64", "cpu_count": 16},
  "samples": 100,
  "time_budget_sec": 1.0,
  "sample_target_sec": 0.01,
  "isolation": "process",
  "cases": 1,
  "failed": 1,
  "time_sec": 1.204,
  "benchmarks": [
    {
      "suite": "Entity",
      "case": "new",
      "status": "ok",
      "iterations": 397000,
      "samples": 100,
      "total_iterations": 39700000,
      "mean_ns": 25.402, "median_ns": 25.310, "stddev_ns": 0.641,
      "min_ns": 24.900, "max_ns": 28.100,
      "p95_ns": 26.400, "p99_ns": 27.800,
      "ci_low_ns": 25.180, "ci_high_ns": 25.440,
      "ci_level": 0.95, "ci_method": "normal-approx-stddev",
      "outliers": 4, "outliers_severe": 1,
      "items_per_iter": 1, "items_per_sec": 39366898.0,
      "time_sec": 1.204,
      "counters": [{"name": "entities", "total": 39700000.0, "per_iter": 1.0}]
    }
  ],
  "failures": [
    {"suite": "Entity", "case": "delete", "status": "crash", "signal": 11, "time_sec": 0.052}
  ]
}
```

`items_per_sec` is only written when `bench_set_items` was called, and
`baseline_median_ns` and `change` are added per case when `--baseline` is used.

`benchmarks` holds the cases that produced a result, with `status` `ok`, and
`cases` counts them. `failures` holds the cases that did not, and `failed`
counts them. A failure has a `status` of `crash`, `timeout` or `error`, the
`signal` that ended the process (POSIX crashes) or its `exit_code` when it is not
zero, and the wall-clock `time_sec` the case took until it ended. `isolation` is
`process`, or `none` for a run with `--in-process`.

## Build reports
`--build-json <file>` writes one json document that describes the build bake
just ran, with a timing for every step. It is accepted by `bake`, `bake build`,
`bake rebuild`, `bake run`, `bake test` and `bake bench`, and describes the
build those commands trigger, not the program or the test run that follows:

```sh
bake3 build my_app --build-json /tmp/build.json
bake3 test test/core --local-env --build-json /tmp/test-build.json -- -j 12
```

The path may be relative (it is resolved against the working directory) and
missing parent directories are created. The report is written whether the build
succeeds or fails: on a failure the top level `ok` is `false`, the step that
failed has `"ok": false` and an `error`, and every step that contains it is
`"ok": false` as well.

```json
{
  "tool": "bake3",
  "tool_version": "1.0.0",
  "timestamp": "2026-09-18T07:50:34Z",
  "host": {"os": "Darwin", "arch": "arm64", "cpu_count": 16},
  "cfg": "debug",
  "target": "arm64-Darwin",
  "environment": "global",
  "git": {"sha": "0c077ef...", "dirty": false, "branch": "main"},
  "total_sec": 1.204312,
  "ok": true,
  "totals": {
    "kind": {
      "compile": 0.812044, "link": 0.238110, "bundle": 0.000000,
      "discovery": 0.004301, "generate": 0.000512, "etc": 0.000188,
      "other": 0.000000
    },
    "project": {
      "my_app": {
        "total_sec": 1.021355, "compile_sec": 0.812044,
        "link_sec": 0.238110, "files": 12
      }
    }
  },
  "steps": [
    {
      "name": "discovery", "kind": "discovery", "project": null,
      "start_sec": 0.000102, "duration_sec": 0.004301, "ok": true,
      "children": [
        {"name": "/home/me/work", "kind": "discovery", "project": null,
         "start_sec": 0.000104, "duration_sec": 0.003912, "ok": true,
         "children": []},
        {"name": "resolve dependencies", "kind": "discovery", "project": null,
         "start_sec": 0.004016, "duration_sec": 0.000387, "ok": true,
         "children": []}
      ]
    },
    {
      "name": "my_app", "kind": "project", "project": "my_app",
      "start_sec": 0.004501, "duration_sec": 1.021355, "ok": true,
      "children": [
        {"name": "bake_config.h", "kind": "generate", "project": "my_app",
         "start_sec": 0.004612, "duration_sec": 0.000512, "ok": true,
         "children": []},
        {"name": "src/main.c", "kind": "compile", "project": "my_app",
         "start_sec": 0.005230, "duration_sec": 0.412009, "ok": true,
         "object": "/home/me/work/my_app/.bake/arm64-Darwin-debug/obj/src/main.c.o",
         "children": []},
        {"name": "link", "kind": "link", "project": "my_app",
         "start_sec": 0.787620, "duration_sec": 0.238110, "ok": true,
         "children": []},
        {"name": "etc", "kind": "etc", "project": "my_app",
         "start_sec": 1.025731, "duration_sec": 0.000188, "ok": true,
         "children": []}
      ]
    }
  ]
}
```

Top level fields:

- `tool`, `tool_version`: the tool that wrote the report.
- `timestamp`: utc time the invocation started.
- `host`: the machine bake ran on.
- `cfg`: the build mode (`--cfg`, `debug` by default).
- `target`: `em` for the emscripten target, otherwise the native `<arch>-<os>`
  triplet the artefacts were built for.
- `environment`: `global`, `local` for `--local-env`, or the name for
  `--local-env=<name>`.
- `git`: `sha`, `dirty` and `branch` of the working directory when it is inside
  a git repository, `null` when it is not. `branch` is `HEAD` on a detached
  head.
- `total_sec`: wall clock of the invocation, from just after bake collected the
  report header to the moment the build finished.
- `ok`: whether the build succeeded.

### Steps
`steps` is a tree. Every step has `name`, `kind`, `project` (`null` when the
step is not part of one project), `start_sec` (offset from the start of the
invocation), `duration_sec`, `ok`, an optional `error`, and `children`. Steps
are listed in the order they started, and a parallel compile keeps its own wall
clock start and duration, so sibling compiles overlap.

- `discovery`: the workspace scan (one per scanned root, named after the root)
  and the `resolve dependencies` pass, nested under one `discovery` step.
- `bundle`: one step per bundle of a project, with a `fetch` child when the
  bundle was cloned and a `build` child when it was (re)built.
- `project`: a container for everything bake did for one project. Its
  `duration_sec` is the project's share of the build.
- `generate`: generated code. `test harness main` / `bench harness main` and
  `test api` / `bench api` for test and benchmark projects, `bake_config.h` for
  every project, plus `rules`, `amalgamate` and `standalone deps` when the
  project uses them.
- `compile`: one step per source file that was actually compiled, named after
  the source path relative to the project, with the `object` path it produced.
  Files that were up to date do not appear. When the project has a `lint`
  section the step includes the lint command, and a file that fails the lint
  has `ok` set to false with the error `lint failed`.
- `link`: the link (or `ar`) command of the project. For the emscripten target
  it has an `embed` child when the project embeds assets: emcc embeds them as
  part of the link, so that step records what is embedded and only times the
  arguments bake composes for it.
- `etc`: the install of the project's `etc` folder into the bake environment.
- `loc`: the lines of code of a project, counted with `cloc` when it is
  installed. The counts are reported in `totals.project.<id>.loc` and
  `totals.loc`. Set `BAKE_BUILD_REPORT_LOC=0` to skip the count.

### Totals
`totals` repeats the same numbers in aggregated form, so a page can show
summaries without walking the tree.

`totals.kind` sums `duration_sec` per kind over the steps that have no
children, so container steps (a `project`, a `bundle` with a fetch and a build)
are never counted twice. Because parallel compiles are timed individually,
`totals.kind.compile` is the sum of the compiles and can exceed the wall clock
of the build.

`totals.project` holds one entry per project: `total_sec` (the time of the
steps bake spent on that project, which includes its bundles), `compile_sec`,
`link_sec` and `files`, the number of source files that were compiled.

### Repeated builds
`--repeat <n>` runs a `build` or `rebuild` `n` times and writes the report of
the median run to `--build-json`, which it requires. `--warmup <n>` adds `n`
runs before them that are not measured, so caches are warm when the measured
runs start. `--warmup` without `--repeat` measures a single run:

```sh
bake3 rebuild --local-env --repeat 3 --warmup 1 -j 1 --build-json /tmp/build.json
```

Every run is a separate bake invocation with the same arguments, so each one
does its own discovery, clean and build, exactly like running the command `n`
times. The runs are compared by `total_sec` minus `totals.kind.loc`: lines of
code are only counted in the first run (the first warm-up when there is one),
and the counts of that run are copied into the reported one. When the reported
run is not the first run, it has no `loc` step and `totals.kind.loc` is 0, so
`total_sec - totals.kind.loc` is the build time of the run in both cases. With
an even count the lower of the two middle runs is the median.

The report gets a `repeat` object:

```json
"repeat": {
  "warmup": 1,
  "count": 3,
  "chosen_run": 2,
  "median_sec": 5.106315,
  "runs": [
    {"run": 0, "warmup": true, "ok": true, "exit_code": 0, "total_sec": 5.342540, "loc_sec": 0.217835},
    {"run": 1, "warmup": false, "ok": true, "exit_code": 0, "total_sec": 5.113472, "loc_sec": 0},
    {"run": 2, "warmup": false, "ok": true, "exit_code": 0, "total_sec": 5.106315, "loc_sec": 0},
    {"run": 3, "warmup": false, "ok": true, "exit_code": 0, "total_sec": 5.138264, "loc_sec": 0}
  ]
}
```

- `warmup`, `count`: the `--warmup` and `--repeat` values.
- `chosen_run`: index in `runs` of the run the report describes.
- `median_sec`: the median of `total_sec - loc_sec` over the measured runs.
- `runs`: every run in the order it ran, with its exit code, `total_sec` and
  the time it spent counting lines of code.

The repetition stops at the first run that fails. The report of that run is
written with `chosen_run` and `median_sec` set to `null`, and bake exits with
an error.

## Project discovery
When bake is called on a directory, it will recursively discover all other bake projects in that directory. A bake project is identified as a project with a `project.json`. The command specified on the bake command line will then be executed for all discovered projects.

When projects have dependencies on each other, bake will ensure that they are built in the correct dependency order.

If one of the discovered projects have a dependency on a project that is not discovered in the specified directory, the binary of the project will be looked up in the bake environment (see below).

If the project also cannot be found in the bake environment, the build cannot proceed, and an error will be thrown.

## Bake Environment
When a project is built with bake, an entry for it will be stored in the bake environment. The location of the bake environment is read from the `BAKE_HOME` environment variable. If the variable is not set, `~/bake3` is used.

When `--local-env` is used, bake overrides `BAKE_HOME` for that invocation to `./.bake/local_env`. When `--local-env=<name>` is used, bake overrides it to `./.bake/local_env/<name>`.

The bake environment has the following directories:

- `<arch-os>/<config>/bin`: stores application binaries
- `<arch-os>/<config>/lib`: stores library binaries
- `include/<project>`: stores the `include` folder of a project
- `etc/<project>`: stores the `etc` folder of a project
- `meta/<project>`: stores project metadata

Processes started by `bake run`, `bake test` and `bake bench` inherit two
variables that tell them which environment they were started from:

- `BAKE_HOME`: the environment root. `~/bake3` for the global environment,
  `<workspace>/.bake/local_env` for `--local-env` and
  `<workspace>/.bake/local_env/<name>` for `--local-env=<name>`. Assets of a
  project are found at `$BAKE_HOME/etc/<project id>`.
- `BAKE_ENVIRONMENT`: the name of the environment. The name for
  `--local-env=<name>`, `local` for `--local-env` without a name, and `global`
  for the global environment.

### Named local environments
See [Orchestrating multiple agents](#orchestrating-multiple-agents) for how this
combines with `bake ps`.

`--local-env=<name>` gives a workspace more than one isolated environment, which lets several people or agents build the same checkout at the same time without sharing build state. Everything the environment owns is scoped by name:

- `.bake/local_env/<name>/build/<project-id>/<arch-os-config>`: object files, generated files and the built artefact
- `.bake/local_env/<name>/<arch-os>/<config>/bin/<project-id>`: installed application binaries
- `.bake/local_env/<name>/<arch-os>/<config>/lib`: installed library binaries
- `.bake/local_env/<name>/{meta,include,test}`: project metadata, installed public headers and test harness templates

A name may contain letters, digits, `.`, `_` and `-`. Names that would collide with a directory of the unnamed environment are rejected: `bin`, `build`, `etc`, `include`, `lib`, `meta`, `src`, `test`, and platform directories such as `arm64-Darwin`.

Named environments do not read or write each other, and they do not read or write the unnamed `.bake/local_env` environment, so `--local-env` without a name keeps behaving exactly as before. `build`, `run`, `test` and `clean` all resolve binaries in the environment of the name they were given, so `bake run <target> --local-env=<name>` runs that name's binary. What *is* shared between names is everything that is expensive and identical for all of them: the bundle sources and bundle builds under `.bake/bundles` (see [Bundles](#bundles)). A second name therefore only pays for compiling the project itself.

Two named builds of the same workspace can run concurrently. Bake serializes the parts that write to shared paths (bundle fetch and build, test harness generation) with a lock, and keeps everything else in per-name directories.

Use `bake run <target> --local-env=<name>` to run a name's binary, `bake test <target> --local-env=<name>` to run its tests, and `bake clean <target> --local-env=<name>` to remove only that name's build output. To see what a name is currently running, or to stop it, use [`bake ps`](#listing-running-processes) and `bake ps --kill <name>`.

A project meta folder stores:
- `project.json`: Copy of the bake configuration for the project
- `LICENSE`: Copy of the license file of the project
- `source.txt`: File with the location of the last location from which the project was built
- `dependee.json`: Project configuration to apply to dependees of the project (copy of the `dependee` section in the project's project.json)

## Building bake
To build bake, run the following command in the repository root:

```
make -j 8
```

Then run this command to initialize the bake environment:

```
./build/bake setup
```

## Testing
To test bake, run `python3 run_tests.py` in the root folder of the project.

Each test removes the workspace it created under `test/tmp`. Set
`BAKE_TEST_KEEP_TMP=1` to keep those trees for debugging.

The suite also builds and runs the C unit tests that cover bake's path, string
and list helpers. To run them on their own:

```
make unit
./build/bake_unit_tests
```
