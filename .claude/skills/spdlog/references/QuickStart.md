```c++
#include "spdlog/spdlog.h"
int main()
{
    //Use the default logger (stdout, multi-threaded, colored)
    spdlog::info("Hello, {}!", "World");
}
```

spdlog provides a python like formatting API using the bundled [fmt](https://github.com/fmtlib/fmt) lib (see the [reference](https://fmt.dev/latest/syntax.html)):
```c++
logger->info("Hello {} {} !!", "param1", 123.4);
```
spdlog takes the "include what you need" approach - your code should include the features that are actually needed.

## Logger Registry

spdlog maintains a global registry of named loggers:

```c++
// Create and auto-register
auto logger = spdlog::basic_logger_mt("my_logger", "logs/my.txt");

// Retrieve later by name
auto logger = spdlog::get("my_logger");

// Explicit registration
spdlog::register_logger(some_logger);

// Set level for ALL registered loggers
spdlog::set_level(spdlog::level::debug);

// Set level for default logger only
spdlog::default_logger()->set_level(spdlog::level::trace);

// Replace the default logger
auto new_default = spdlog::basic_logger_mt("new_default", "logs/new.txt", true);
spdlog::set_default_logger(new_default);

// Remove specific logger from registry
spdlog::drop("my_logger");

// Remove all loggers
spdlog::drop_all();
```

For example, if you only need rotating logger, you need to include "**spdlog/sinks/rotating_file_sink.h**".

Another example would be to include "**spdlog/async.h**" to get asynchronous logging features.


####  Basic example
```c++
#include <iostream>
#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h" // support for basic file logging
#include "spdlog/sinks/rotating_file_sink.h" // support for rotating file logging

int main(int, char* [])
{
    try 
    {
        // Create basic file logger (not rotated)
        auto my_logger = spdlog::basic_logger_mt("basic_logger", "logs/basic.txt");
        
        // create a file rotating logger with 5mb size max and 3 rotated files
        auto file_logger = spdlog::rotating_logger_mt("file_logger", "myfilename", 1024 * 1024 * 5, 3);
    }
    catch (const spdlog::spdlog_ex& ex)
    {
        std::cout << "Log initialization failed: " << ex.what() << std::endl;
    }
}
```

#### Create an asynchronous logger using factory method
```c++
#include <iostream>
#include "spdlog/spdlog.h"
#include "spdlog/async.h" //support for async logging.
#include "spdlog/sinks/basic_file_sink.h"

int main(int, char* [])
{
    try
    {        
        auto async_file = spdlog::basic_logger_mt<spdlog::async_factory>("async_file_logger", "logs/async_log.txt");
        for (int i = 1; i < 101; ++i)
        {
            async_file->info("Async message #{}", i);
        }
        // Under VisualStudio, this must be called before main finishes to workaround a known VS issue
        spdlog::drop_all(); 
    }
    catch (const spdlog::spdlog_ex& ex)
    {
        std::cout << "Log initialization failed: " << ex.what() << std::endl;
    }
}
```

#### Create an asynchronous logger and change thread pool settings
```c++
#include <iostream>
#include "spdlog/spdlog.h"
#include "spdlog/async.h" //support for async logging
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/daily_file_sink.h"
int main(int, char* [])
{
    try
    {                                        
        auto daily_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>("logfile", 23, 59);
        // default thread pool settings can be modified *before* creating the async logger:
        spdlog::init_thread_pool(10000, 1); // queue with 10K items and 1 backing thread.
        auto async_file = spdlog::basic_logger_mt<spdlog::async_factory>("async_file_logger", "logs/async_log.txt");       
        spdlog::drop_all(); 
    }
    catch (const spdlog::spdlog_ex& ex)
    {
        std::cout << "Log initialization failed: " << ex.what() << std::endl;
    }
}
```


####  Create multiple loggers that share the same file (sink) aka categories
```c++
#include <iostream>
#include "spdlog/spdlog.h"
#include "spdlog/sinks/daily_file_sink.h"
int main(int, char* [])
{
    try
    {
        auto daily_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>("logfile", 23, 59);
        // create synchronous  loggers
        auto net_logger = std::make_shared<spdlog::logger>("net", daily_sink);
        auto hw_logger  = std::make_shared<spdlog::logger>("hw",  daily_sink);
        auto db_logger  = std::make_shared<spdlog::logger>("db",  daily_sink);      

        net_logger->set_level(spdlog::level::critical); // independent levels
        hw_logger->set_level(spdlog::level::debug);
         
        // globally register the loggers so they can be accessed using spdlog::get(logger_name)
        spdlog::register_logger(net_logger);
    }
    catch (const spdlog::spdlog_ex& ex)
    {
        std::cout << "Log initialization failed: " << ex.what() << std::endl;
    }
}
```

####  Create a logger with multiple sinks, each sink with its own formatting and log level

```c++
//
// Logger with console and file output.
// the console will show only warnings or worse, while the file will log all messages.
// 
#include <iostream>
#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h" // or "../stdout_sinks.h" if no colors needed
#include "spdlog/sinks/basic_file_sink.h"
int main(int, char* [])
{
    try
    {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(spdlog::level::warn);
        console_sink->set_pattern("[multi_sink_example] [%^%l%$] %v");

        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/multisink.txt", true);
        file_sink->set_level(spdlog::level::trace);

        spdlog::sinks_init_list sink_list = { file_sink, console_sink };

        spdlog::logger logger("multi_sink", sink_list.begin(), sink_list.end());
        logger.set_level(spdlog::level::debug);
        logger.warn("this should appear in both console and file");
        logger.info("this message should not appear in the console, only in the file");

        // or you can even set multi_sink logger as default logger
        spdlog::set_default_logger(std::make_shared<spdlog::logger>("multi_sink", spdlog::sinks_init_list({console_sink, file_sink})));

    }
    catch (const spdlog::spdlog_ex& ex)
    {
        std::cout << "Log initialization failed: " << ex.what() << std::endl;
    }
}
```

#### Logging macros
You can define SPDLOG_ACTIVE_LEVEL to the desired log level *before* including "spdlog.h".

This will turn on/off logging statements at compile time
```c++
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG

spdlog::set_level(spdlog::level::debug); // or spdlog::set_level(spdlog::level::trace); 

SPDLOG_LOGGER_TRACE(file_logger , "Some trace message that will not be evaluated.{} ,{}", 1, 3.23);
SPDLOG_LOGGER_DEBUG(file_logger , "Some Debug message that will be evaluated.. {} ,{}", 1, 3.23);
SPDLOG_DEBUG("Some debug message to default logger that will be evaluated");
```

Notice that `spdlog::set_level` is also necessary to print out debug or trace messages.

#### Log user-defined objects
```c++
#include "spdlog/spdlog.h"
#include "spdlog/fmt/ostr.h" // must be included
#include "spdlog/sinks/stdout_sinks.h"

class some_class {};
std::ostream& operator<<(std::ostream& os, const some_class& c)
{ 
    return os << "some_class"; 
}

// fmt v10 and above requires `fmt::formatter<T>` extends `fmt::ostream_formatter`.
// See: https://github.com/fmtlib/fmt/issues/3318
template <> struct fmt::formatter<some_class> : fmt::ostream_formatter {};

void custom_class_example()
{
    some_class c;
    auto console = spdlog::stdout_logger_mt("console");
    console->info("custom class with operator<<: {}..", c);
}
```
or use the bundled `fmt` lib when advanced formatting is needed:
```c++
#include <iterator>

#include "spdlog/spdlog.h"
#include "spdlog/fmt/ostr.h" // must be included
#include "spdlog/sinks/stdout_sinks.h"

class some_class {
    int code;
};

template<typename OStream>
friend OStream &operator<<(OStream &os, const some_class& to_log)
{
    fmt::format_to(std::ostream_iterator<char>(os), "{:04X}", to_log.code);
    return os;
}

void custom_class_example()
{
    some_class c; c.code = 17;
    auto console = spdlog::stdout_logger_mt("console");
    console->info("custom class with operator<< using fmt: {}..", c);
}
```

## Backtrace Support

Debug messages can be stored in a ring buffer and dumped on demand (e.g., when an error occurs):

```c++
spdlog::enable_backtrace(32); // Store last 32 messages in buffer
// or: my_logger->enable_backtrace(32);

for (int i = 0; i < 100; i++) {
    spdlog::debug("Backtrace message {}", i); // not logged yet
}

// On error, dump the buffer:
spdlog::dump_backtrace(); // logs the last 32 messages now
// or: my_logger->dump_backtrace(32);
```

## Stopwatch

```c++
#include "spdlog/stopwatch.h"

spdlog::stopwatch sw;
spdlog::debug("Elapsed {}", sw);        // default format
spdlog::debug("Elapsed {:.3}", sw);     // 3 decimal places
```

## Binary Data in Hex

```c++
#include "spdlog/fmt/bin_to_hex.h"

std::array<char, 80> buf;
logger->info("Binary example: {}", spdlog::to_hex(buf));
logger->info("First 10 bytes:{:n}", spdlog::to_hex(std::begin(buf), std::begin(buf) + 10));

// Format flags (combine as needed):
// {:X} - uppercase hex
// {:s} - no spaces between bytes
// {:p} - no position info at line start
// {:n} - no line splitting
// {:a} - show ASCII (if :n not set)
```

## Custom Error Handler

```c++
// Set globally or per logger (logger->set_error_handler(..))
spdlog::set_error_handler([](const std::string &msg) {
    spdlog::get("console")->error("*** LOGGER ERROR ***: {}", msg);
});
```

## Load Log Levels from Environment or argv

```c++
#include "spdlog/cfg/env.h"   // for env variable
#include "spdlog/cfg/argv.h"   // for command line

spdlog::cfg::load_env_levels();
// or: spdlog::cfg::load_env_levels("MYAPP_LEVEL");
// or: spdlog::cfg::load_argv_levels(argc, argv);
```

Usage:
```console
$ export SPDLOG_LEVEL=info,mylogger=trace
$ ./myapp
```

## Mapped Diagnostic Context (MDC)

Thread-local key-value storage appended to log output. **Not supported in async mode** (relies on thread-local storage).

```c++
#include "spdlog/mdc.h"

spdlog::mdc::put("key1", "value1");
spdlog::mdc::put("key2", "value2");
// Use %& in pattern to print MDC data:
// spdlog::set_pattern("[%H:%M:%S %z] [%^%L%$] [%&] %v");
```