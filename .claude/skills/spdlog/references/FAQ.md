## The log file remains empty

For performance reasons, log entries are not flushed to files immediately, but only after BUFSIZ bytes have been logged (as defined in libc).

Use any of the following methods to force flush:
```c++
my_logger->flush();                            // flush now
my_logger->flush_on(spdlog::level::info);      // auto flush when "info" or higher message is logged
spdlog::flush_on(spdlog::level::info);         // auto flush when "info" or higher message is logged on all loggers
spdlog::flush_every(std::chrono::seconds(5));  // flush periodically every 5 seconds (caution: must be _mt logger)
```
See the [flush policy](https://github.com/gabime/spdlog/wiki/7.-Flush-policy) for details.

## How to remove all debug statements at compile time ?
Define ```SPDLOG_ACTIVE_LEVEL``` to the desired log level before including spdlog.h and use the SPDLOG_ macros:

```c++
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO // All DEBUG/TRACE statements will be removed by the pre-processor
#include "spdlog/spdlog.h"
...
SPDLOG_DEBUG("debug message to default logger"); // removed at compile time
SPDLOG_LOGGER_TRACE(my_logger, "trace message"); // removed at compile time
SPDLOG_INFO("info message to default logger");   // included
```

Note that this removes the debug statements at compile time, but you still need to set the desired log level at 
runtime to be able to see them.

## How to use external fmt library instead of the bundled?

To use external fmt library, ```SPDLOG_FMT_EXTERNAL``` macro must be defined.

If you build spdlog with CMake, you must define CMake variable ```SPDLOG_FMT_EXTERNAL``` or ```SPDLOG_FMT_EXTERNAL_HO``` (header-only fmt).
```SPDLOG_FMT_EXTERNAL``` macro is automatically defined in spdlog added by CMake.

## Format string compile-time check does not work when there are no args

If only the format string is passed, the compile-time check does not work.
This is because the design does not use a formatter for performance is such case ([#3056](https://github.com/gabime/spdlog/issues/3056)).

```c++
spdlog::info("{}{}", 1, 2); // OK
spdlog::info("{}{}{}", 1, 2); // Compile error.

spdlog::info("{}"); // OK.
```

## Asynchronous logging hangs on application exit under Windows

At the end of main, call  ```spdlog::shutdown();```

## Asynchronous logging hangs under Windows dll

Make sure you don't create the async logger (or the thread pool) inside **DllMain** (e.g. by using static logger variable inside the dll). 

## How to use spdlog in shared library?

See [How to use spdlog in DLLs](https://github.com/gabime/spdlog/wiki/How-to-use-spdlog-in-DLLs) for details.

## Memory leak detected when using inside a shared library

The async logger and ```spdlog::flush_every()``` create a worker thread.

When using these in a shared library, you must call ```spdlog::shutdown();``` before the shared library is unloaded from memory.
Otherwise, the objects associated with the thread may not be released.


## Colors do not appear when using custom format

You need enclose the colored part you want with ```%^ ``` and ```%$```.

For example ```spdlog::set_pattern(“%^[%l]%$ %v”);```

## Source information do not appear when using custom format

Some custom format patterns, such as ```%@```, ```%s```, ```%g```, ```%#```, ```%!```, print the source information.

For these to work, you must use compile time log level macros.

For example:
```cpp

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO 
#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_sinks.h"

void source_info_example()
{
    auto console = spdlog::stdout_logger_mt("console");
    spdlog::set_default_logger(console);
    spdlog::set_pattern("[source %s] [function %!] [line %#] %v");

    SPDLOG_LOGGER_INFO(console, "log with source info"); // Console: "[source example.cpp] [function source_info_example] [line 10] log with source info"
    SPDLOG_INFO("global log with source info"); // Console: "[source example.cpp] [function source_info_example] [line 11] global logger with source info"

    console->info("source info is not printed"); // Console: "[source ] [function ] [line ] source info is not printed"
}
```

## WinAPI `min`/`max` macro definitions

When using the header-only distribution of spdlog together with the bundled fmt library on Windows you end up including `Windows.h` via the bundled fmt library.
To prevent any errors caused by the definiton of `min`/`max` macros in WinAPI you can either

- Surround your `min()` or `max()` calls with parentheses: `(std::max)(n1, n2)` or
- define `NOMINMAX` macro before including spdlog.h, or
- use external fmt library(`-DSPDLOG_FMT_EXTERNAL=ON` when building spdlog)

See [#1553](https://github.com/gabime/spdlog/issues/1553) or [fmt#1508](https://github.com/fmtlib/fmt/issues/1508) for details.

## Switch the default logger to `stderr`

You can `set_default_logger()` with a new `stderr` logger. If you want the logging format to remain unchanged, you should specify the name as an empty string, but this will cause a conflict with the initial default logger, which is already using that name. To resolve this, first replace the default logger with one that has some other arbitrary name&hellip;

```c++
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"
int main()
{
    // Replace the default logger with a (color, single-threaded) stderr logger
    // (but first replace it with an arbitrarily-named logger to prevent a name clash)
    spdlog::set_default_logger( spdlog::stderr_color_st( "some_arbitrary_name" ) );
    spdlog::set_default_logger( spdlog::stderr_color_st( "" ) );
    spdlog::info("This message will go to stderr");
}
```

## How to force log messages to be output (ignore log level filter)

You can use `spdlog::level::off` to force log messages to be output ([Discussion](/gabime/spdlog/discussions/2640)).

```cpp
logger.set_level(spdlog::level::error);

logger.warn("This message is not output.");

logger.log(spdlog::level::off, "This message is output.");
```

Note that this way does not trigger log level-based flush ( `logger.flush_on(spdlog::level)`).

## GCC standard library compatibility problem

If you build a simple code like the following with GCC compiler and get `std::bad_alloc`, you may be using an incompatible standard library.

```cpp
#include "spdlog/spdlog.h"

int main(int argc, char *argv[])
{
    spdlog::info("Test message.");

    return 0;
}
```

`std::bad_alloc` occurs mostly when passing a dangling pointer or a string that has not been null terminated.
However, in GCC only, this error may be reported when spdlog and the application use different standard libraries (e.g. `libstdc++` and `libstdc++11`).
This problem has been reported more often when using the package manager.
If you use a package manager, please check your ABI settings.

See [#2286](https://github.com/gabime/spdlog/issues/2286) or [#2290](https://github.com/gabime/spdlog/issues/2290) for details.