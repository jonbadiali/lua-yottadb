# Overview

This project provides a shared library that lets the [Lua](https://lua.org/) language access a [YottaDB database](https://yottadb.com/) and the means to invoke M routines from within Lua. While this project is stand-alone, there is a closely related project called [MLua](https://github.com/anet-be/mlua) that goes in the other direction, allowing M software to invoke the Lua language. If you wish for both abilities, start with [MLua](https://github.com/anet-be/mlua) which is designed to incorporate lua-yottadb.

**Author:** [Mitchell](https://github.com/orbitalquark) is lua-yottadb's original author, basing it heavily on [YDBPython](https://gitlab.com/YottaDB/Lang/YDBPython). [Berwyn Hoyt](https://github.com/berwynhoyt) took over development to create version 1.0. It is sponsored by, and is copyright © 2022, [University of Antwerp Library](https://www.uantwerpen.be/en/library/).

**License:** Unless otherwise stated, the software is licensed with the [GNU Affero GPL](https://opensource.org/licenses/AGPL-3.0), and the compat-5.3 files are licensed with the [MIT license](https://opensource.org/licenses/MIT).

## Quickstart

If you do not already have it, install [YottaDB here](https://yottadb.com/product/get-started/). Then install lua-yottadb as follows:

```bash
git clone https://github.com/anet-be/lua-yottadb.git
cd lua-yottadb
make
sudo make install
```

## Intro by example

Below are some examples to get you started. When you have exhausted that, read the API [reference documentation here](https://htmlpreview.github.io/?https://github.com/anet-be/lua-yottadb/blob/master/docs/yottadb.html).

Let's tinker with setting some database values in different ways:

```lua
ydb = require 'yottadb'
n = ydb.node('^hello')  -- create node object pointing to YottaDB global ^hello

n:get()  -- get current value of the node in the database
-- nil
n:set('Hello World')
n:get()
-- Hello World

-- Equivalent ways to create a new subnode object pointing to an added 'cowboy' subscript
n2 = ydb.node('^hello')('cowboy')
n2 = ydb.node('^hello', 'cowboy')
n2 = n('cowboy')
n2 = n['cowboy']
n2 = n.cowboy

n2:set('Howdy partner!')  -- set ^hello('cowboy') to 'Howdy partner!'
n2, n2:get()
-- ^hello("cowboy")	Howdy partner!

n2.ranches:set(3)  -- create subnode object ydb.node('^hello', 'cowboy', 'ranches') and set to 3
n2.ranches.__ = 3   -- same as :set() but 3x faster (ugly but direct access to value)

n3 = n.chinese  -- add a second subscript to '^hello', creating a third object
n3:set('你好世界!') -- value can be number or string, including UTF-8
n3, n3:get()
-- hello("chinese")	你好世界!
```

We can also use other [methods of the node object](https://htmlpreview.github.io/?https://github.com/anet-be/lua-yottadb/blob/master/docs/yottadb.html#Class_node) like `incr() name() has_value() has_key() lock_incr()`:

```lua
n2.ranches:incr(2)  -- increment
-- 5
n2:name()
-- cowboy
n2:__name()  -- uglier but 15x faster access to node object methods
-- cowboy
```

(Note: lua-yottadb is able to distinguish n:method(n) from subnode creation n.method. See [details in the notes here](https://htmlpreview.github.io/?https://github.com/anet-be/lua-yottadb/blob/master/docs/yottadb.html#Class_node).)

Now, let's try `dump` to see what we've got so far:

```lua
n:dump()
```

The output will be:

```lua
^hello="Hello World"
^hello("chinese")="你好世界!"
^hello("cowboy")="Howdy partner!"
^hello("cowboy","ranches")="5"
```

We can delete a node -- either its *value* or its entire *tree*:

```lua
n:set(nil)  -- delete the value of '^hello', but not any of its child nodes
n:get()
-- nil
n:dump()  -- the values of the child node are still in the database
-- hello("chinese")="你好世界!!"
-- hello("cowboy")="Howdy partner!"

n:delete_tree() -- delete both the value at the '^hello' node and all of its children
n:dump()
-- nil
```

### Doing something useful

Let's use Lua to calculate the height of 3 oak trees, based on their shadow length and the angle of the sun. Method `settree()` is a handy way to enter literal data into the database from a Lua table constructor:

```lua
trees = ydb.node('^oaks')  -- create node object pointing to YottaDB global ^oaks
-- store initial data values into database subnodes ^oaks('1', 'shadow'), etc.
trees:settree({{shadow=10, angle=30}, {shadow=13, angle=30}, {shadow=15, angle=45}})

for index, oaktree in pairs(trees) do
    oaktree.height.__ = oaktree.shadow.__ * math.tan( math.rad(oaktree.angle.__) )
    print( string.format('Oak %s is %.1fm high', index, oaktree.height.__) )
  end
-- Oak 1 is 5.8m high
-- Oak 2 is 7.5m high
-- Oak 3 is 15.0m high
```

You may also wish to look at **`node:gettree()`** which has multiple uses. On first appearances, it just loads a database tree into a Lua table (opposite of `settree` above), but it also allows you to iterate over a whole database tree and process each node through a filter function. For example, to use `print` as a filter function, do `node:gettree(nil, print)` from the [API docs](https://htmlpreview.github.io/?https://github.com/anet-be/lua-yottadb/blob/master/docs/yottadb.html#node:gettree). Incidentally, lua-yottadb itself uses `gettree` to implement `node:dump()`.

### Database transactions are also available:

```lua
> Znode = ydb.node('^Ztest')
> transact = ydb.transaction(function(end_func)
  print("^Ztest starts as", Znode:get())
  Znode:set('value')
  end_func()
  end)

> transact(ydb.trollback)  -- perform a rollback after setting Znode
^Ztest starts as	nil
YDB Error: 2147483645: YDB_TP_ROLLBACK
> Znode.get()  -- see that the data didn't get set
nil

> tries = 2
> function trier()  tries=tries-1  if tries>0 then ydb.trestart() end  end
> transact(trier)  -- restart with initial dbase state and try again
^Ztest starts as	nil
^Ztest starts as	nil
> Znode:get()  -- check that the data got set after restart
value

> Znode:set(nil)
> transact(function() end)  -- end the transaction normally without restart
^Ztest starts as	nil
> Znode:get()  -- check that the data got set
value
```

### Calling M from Lua

The Lua wrapper for M is designed for both speed and simple usage. The following calls M routines from [arithmetic.m](examples/arithmetic.m):

```lua
$ export ydb_routines=examples   # put arithmetic.m into ydb path
$ lua
> arithmetic = ydb.require('examples/arithmetic.ci')
> arithmetic.add_verbose("Sum is:", 2, 3)
Sum is: 5
Sum is: 5
> arithmetic.sub(5,7)
-2
```

Lua parameter types are converted to ydb types automatically according to the call-in table arithmetic.ci. If you need speed, avoid returning or outputting strings from M as they require the speed hit of memory allocation.

Note that the filename passed to ydb.require() may be either a call-in table filename or (if the string contains a `:`) a string specifying actual call-in routines. Review file [examples/arithmetic.ci](examples/arithmetic.ci) and the [ydb manual](https://docs.yottadb.com/ProgrammersGuide/extrout.html#call-in-table) for details about call-in table specification.

### Development aids

You can enhance Lua to display database nodes or table contents when you type them at the Lua prompt. This project supplies a [`startup.lua`](examples/startup.lua) file to make this happen. To use it, simply set the environment variable:
 `export LUA_INIT=@path-to-lua-yottadb/examples/startup.lua`

Now Lua tables and database nodes display their contents when you type them at the Lua REPL prompt:

```lua
> t={test=5, subtable={x=10, y=20}}
> t
test: 5
subtable (table: 0x56494c7dd5b0):
  x: 10
  y: 20
> n=ydb.node('^oaks')
> n:settree({__='treedata', {shadow=10,angle=30}, {shadow=13,angle=30}})
> n
^oaks="treedata"
^oaks("1","angle")="30"
^oaks("1","shadow")="10"
^oaks("2","angle")="30"
^oaks("2","shadow")="13"
```

## Technical details

Version history and current version number is documented at the top of [yottadb.h](yottadb.h).

### Reference docs

The [API reference documentation](https://htmlpreview.github.io/?https://github.com/anet-be/lua-yottadb/blob/master/docs/yottadb.html), is generated from source file comments. To update them, run `make docs` and commit changes to the resulting files `docs/*.html`. Prerequisite: `luarocks install ldoc`. There is also [documentation for the underlying C functions](https://htmlpreview.github.io/?https://github.com/anet-be/lua-yottadb/blob/master/docs/yottadb_c.html), but keep in mind that the C interface is not part of the public API and may change.

### Thread Safety

Lua co-routines, are perfectly safe to use with YDB, since they are cooperative rather than preemptive.

However, lua-yottadb does not currently support multi-threaded applications – which would require accessing YDB using [threaded C API functions](https://docs.yottadb.com/MultiLangProgGuide/programmingnotes.html#threads) (unless the application designer ensures that only one of the threads accesses the database). If there is keen demand, it shouldn't be too difficult to upgrade lua-yottadb to use the thread-safe function calls, making lua-yottadb thread-safe. (Lua would still requires, though, that each thread running Lua do so in a separate lua_State).

### Signals & EINTR errors

Your Lua code must treat signals with respect. The the first use of YDB will set up several YDB signals which may interrupt your subsequent Lua code. If your Lua code doesn't use slow or blocking IO like user input or pipes then you should have nothing to worry about. But if you're getting `Interrupted system call` (EINTR) errors from Lua, then you need to read this section.

YDB uses signals heavily (especially SIGALRM: see below). This means that YDB signal/timer handlers may be called while running Lua code. This doesn't matter until your Lua code waits on IO operations (using read/write/open/close), in which case these operations will return the EINTR error if a YDB signal occurs while they are waiting on IO. Lua C code itself is not written to retry this error condition, so your software will fail unnecessarily unless you handle them. If you really *do* wish to handle EINTR errors yourself, you should also call YDB API function [ydb_eintr_handler()](https://docs.yottadb.com/MultiLangProgGuide/cprogram.html#ydb-eintr-handler-ydb-eintr-handler-t) whenever you get an EINTR error.

Lua-yottadb offers a mechanism to resolve this automatically by blocking YDB signals until next time it calls YDB. To use it, simply call `yottadb.init(yottadb.block_M_signals)` before running your code. Be aware that if you use signal blocking with long-running Lua code, the database will not run timers until your Lua code returns (though it can flush database buffers: see the note on SIGALRM below). Also note that setting up signal blocking is slow, so using `block_M_signals` will increase the M calling overhead by about 1.4 microseconds on my machine: 2-5x the bare calling overhead without blocking (see `make benchmarks`).

#### Specific signals

Your Lua code must not use any of the following [YDB Signals](https://docs.yottadb.com/MultiLangProgGuide/programmingnotes.html#signals), which are the same ones that `block_M_signals` blocks:

- SIGALRM: used for [M Timeouts](https://docs.yottadb.com/ProgrammersGuide/langfeat.html#timeouts), [$ZTIMEOUT](https://docs.yottadb.com/ProgrammersGuide/isv.html#ztimeout), [$ZMAXTPTIME](https://docs.yottadb.com/ProgrammersGuide/isv.html#zmaxtptime), device timeouts, [ydb_start_timer()](https://docs.yottadb.com/MultiLangProgGuide/cprogram.html#ydb-timer-start-ydb-timer-start-t) API, and a buffer flush timer roughly once every second.
  - Note that although most signals are completely blocked (using [sigprocmask](https://man7.org/linux/man-pages/man2/sigprocmask.2.html)), lua-yottadb doesn't actually block SIGALRM; instead it temporarily sets its SA_RESTART flag (using [sigaction](https://man7.org/linux/man-pages/man2/sigaction.2.html)) so that the OS automatically restarts IO calls that were interrupted by SIGALRM. The benefit of this over blocking is that the YDB SIGALRM handler does actually run, allowing it still to flush the database or IO as necessary without your M code having to call the M command `VIEW "FLUSH"`.
- SIGCHLD: wakes up YDB when a child process terminates.
- SIGTSTP, SIGTTIN, SIGTTOU, SIGCONT: YDB handles these to defer suspension until an opportune time.
- SIGUSR1: used for [$ZINTERRUPT](https://docs.yottadb.com/ProgrammersGuide/isv.html#zinterrupt)
- SIGUSR2: used only by GT.CM servers, the Go-ydb wrapper, or env variable `ydb_treat_sigusr2_like_sigusr1`.

If you really do need to use these signals, you would have to understand how YDB initialises and uses them and make your handler call its handler, as appropriate. This is not recommended.

Note that lua-yottadb does **not** block SIGINT or fatal signals: SIGQUIT, SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGIOT, SIGSEGV, SIGTERM, and SIGTRAP. Instead lua-yottadb lets the YDB handlers of these terminate the program as appropriate.

You may use any other signals, but first read [Limitations on External Programs](https://docs.yottadb.com/ProgrammersGuide/extrout.html#limitations-on-the-external-program) and [YDB Signals](https://docs.yottadb.com/MultiLangProgGuide/programmingnotes.html#signals). Be aware that:

- You will probably want to initialise your signal handler using the SA_RESTART flag so that the OS automatically retries long-running IO calls (cf. [system calls that can return EINTR](https://man7.org/linux/man-pages/man7/signal.7.html#:~:text=interruption of system calls and library functions by signal handlers)). You may also wish to know that the only system calls that Lua 5.4 uses which can return EINTR are (f)open/close/read/write. These are only used by Lua's `io` library. However, third-party libraries may use other system calls (e.g. posix and LuaSocket libraries).
- If you need an OS timer, you cannot use `setitimer()` since it can only trigger SIGALRM, which YDB uses. Instead use `timer_create()` to trigger your own signal.

If you need more detail on signals, here is a discussion in the MLua repository of the [decision to block YDB signals](https://github.com/anet-be/mlua/discussions/8). It also describes why SA_RESTART is not a suitable solution for YDB itself when running M code.

## Installation Detail

### Requirements

* YottaDB 1.34 or later.
* Lua 5.1 or greater. The Makefile will use your system's Lua version by default. To override this, run `make lua=/path/to/lua`.
  * Lua-yottadb has been built and tested with every major Lua version from 5.1 onward ([MLua](https://github.com/anet-be/mlua) does this with `make testall`)

If more specifics are needed, build the bindings by running `make ydb_dist=/path/to/YDB/install` where */path/to/YDB/install* is the path to your installation of YottaDB that contains its header and shared library files.

Install the bindings by running `make install` or `sudo make install`, or copy the newly built
*_yottadb.so* and *yottadb.lua* files to somewhere in your Lua path. You can then use `local
yottadb = require('yottadb')` from your Lua scripts to communicate with YottaDB.  Then set up
your YDB environment as usual (i.e. source /path/to/YDB/install/ydb_env_set) before running
your Lua scripts.

The *Makefile* looks for Lua header files either in your compiler's default include path such
as /usr/local/include (the lua install default) or in */usr/include/luaX.Y*. Where *X.Y*
is the lua version installed on your system. If your Lua headers are elsewhere, run
`make lua_include=/path/to/your/lua/headers`.

### Example install of yottadb with minimal privileges

The following example builds and installs a local copy of YottaDB to *YDB/install* in the current
working directory. The only admin privileges required are to change ownership of *gtmsecshr*
to root, which YottaDB requires to run. (Normally, YottaDB's install script requires admin
privileges.)

```bash
# Install dependencies.
sudo apt-get install --no-install-recommends {libconfig,libelf,libgcrypt,libgpgme,libgpg-error,libssl}-dev
# Fetch YottaDB.
ydb_distrib="https://gitlab.com/api/v4/projects/7957109/repository/tags"
ydb_tmpdir='tmpdir'
mkdir $ydb_tmpdir
wget -P $ydb_tmpdir ${ydb_distrib} 2>&1 1>${ydb_tmpdir}/wget_latest.log
ydb_version=`sed 's/,/\n/g' ${ydb_tmpdir}/tags | grep -E "tag_name|.pro.tgz" | grep -B 1 ".pro.tgz" | grep "tag_name" | sort -r | head -1 | cut -d'"' -f6`
git clone --depth 1 --branch $ydb_version https://gitlab.com/YottaDB/DB/YDB.git
# Build YottaDB.
cd YDB
mkdir build
cd build/
cmake -D CMAKE_INSTALL_PREFIX:PATH=$PWD -D CMAKE_BUILD_TYPE=Debug ../
make -j4
make install
cd yottadb_r132
nano configure # replace 'root' with 'mitchell'
nano ydbinstall # replace 'root' with 'mitchell'
# Install YottaDB locally.
./ydbinstall --installdir /home/mitchell/code/lua/lua-yottadb/YDB/install --utf8 default --verbose --group mitchell --user mitchell --overwrite-existing
# may need to fix yottadb.pc
cd ../..
chmod ug+w install/ydb_env_set # change '. $ydb_tmp_env/out' to 'source $ydb_tmp_env/out'
sudo chown root.root install/gtmsecshr
sudo chmod a+s install/gtmsecshr

# Setup env.
source YDB/install/ydb_env_set

# Run tests with gdb.
ydb_gbldir=/tmp/lydb.gld gdb lua
b set
y
r -llydb tests/test.lua
```

