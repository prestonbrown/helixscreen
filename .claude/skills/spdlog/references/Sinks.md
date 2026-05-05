Sinks are the objects that actually write the log to their target. Each sink should be responsible for only single target (e.g file, console, db), and each sink has its own private instance of formatter object.

Each logger contains a vector of one or more```std::shared_ptr<sink>```. On each log call (if the log level is right) the logger will call the ```"sink(log_msg)"``` function on each of them.


spdlog's sinks have _mt (multi threaded) or _st (single threaded) suffixes to indicate the thread safety.
While single threaded sinks cannot be used from multiple threads simultaneously, they are faster because no locking is employed.


### Available Sinks

***Note*** This is only a partial list. For the full list of sinks please visit the [sinks folder](https://github.com/gabime/spdlog/tree/v1.x/include/spdlog/sinks/).


***Note:*** Starting from version 1.5.0, spdlog will create, if needed, the folders that contain the log file. Prior to this, the folders must be created manually.
***

**rotating_file_sink** 

When the max file size is reached, close the file, rename it, and create a new file. Both the max file size and the max number of files are configurable in the constructor.

```c++
// create a thread safe sink which will keep its file size to a maximum of 5MB and a maximum of 3 rotated files.
#include "spdlog/sinks/rotating_file_sink.h"
...
auto file_logger = spdlog::rotating_logger_mt("file_logger", "logs/mylogfile", 1048576 * 5, 3);
```
or create the sink manually and pass it to the logger:
```c++
#include "spdlog/sinks/rotating_file_sink.h"
...
auto rotating = make_shared<spdlog::sinks::rotating_file_sink_mt> ("log_filename", 1024*1024, 5, false);
auto file_logger = make_shared<spdlog::logger>("my_logger", rotating);
```

***
**daily_file_sink**

Create a new log file every day at the specified time, appending a timestamp to the filename.

```c++
#include "spdlog/sinks/daily_file_sink.h"
..
auto daily_logger = spdlog::daily_logger_mt("daily_logger", "logs/daily", 14, 55);
```
will create a thread safe sink which will create a new log file each day on 14:55.

***Note:*** Starting from version 1.5.0, spdlog will create, if needed, the folders that contain the log file. Prior to this, the folders must be created manually.

***Note:*** 
No old log files will be deleted on startup, even if they would've been cleared if created during runtime. Rotation and deletion only happens at runtime, not when the sink is created.

***
**simple_file_sink** 

A simple file sink that just writes to the given log file with no restrictions.

***
```c++
#include "spdlog/sinks/basic_file_sink.h"
...
auto logger = spdlog::basic_logger_mt("mylogger", "log.txt");
```
***Note:*** Starting from version 1.5.0, spdlog will create, if needed, the folders that contain the log file. Prior to this, the folders must be created manually.

***
**stdout_sink/stderr_sink** 

```c++
#include "spdlog/sinks/stdout_sinks.h"
...
auto console = spdlog::stdout_logger_mt("console");
auto err_console = spdlog::stderr_logger_st("console");
```

**stdout_sink/stderr_sink with colors**  

```c++
#include "spdlog/sinks/stdout_color_sinks.h"
...
auto console = spdlog::stdout_color_mt("console");
auto err_console = spdlog::stderr_color_st("console");
```

Or create the sinks directly:
```c++
auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
```

***
**ostream_sink** 

```c++
#include "spdlog/sinks/ostream_sink.h "
...
std::ostringstream oss;
auto ostream_sink = std::make_shared<spdlog::sinks::ostream_sink_mt> (oss);
auto logger = std::make_shared<spdlog::logger>("my_logger", ostream_sink);
```

***
**null_sink**:

null sink that throws away its log - can be used for debugging or as a reference implementation.

```c++
#include "spdlog/sinks/null_sink.h"
...
auto logger = spdlog::create<spdlog::sinks::null_sink_st>("null_logger");
```

***
**syslog_sink** 

POSIX syslog(3) sink which sends its log to syslog. 
```c++
#include "spdlog/sinks/syslog_sink.h"
...
auto syslog_sink = std::make_shared<spdlog::sinks::syslog_sink_mt>("my_ident", LOG_PID);
auto syslog_logger = std::make_shared<spdlog::logger>("logger_name", syslog_sink);
```

**systemd_sink** 

sink which sends its log to systemd
```c++
#include "spdlog/sinks/systemd_sink.h"
...
auto systemd_sink = std::make_shared<spdlog::sinks::systemd_sink_st>();
auto systemd_logger = std::make_shared<spdlog::logger>("logger_name", systemd_sink);
```

***
**dist_sink** ([sinks/dist_sink.h](https://github.com/gabime/spdlog/tree/master/include/spdlog/sinks/dist_sink.h)):

Distribute log messages to a list of other sinks:

```c++
#include "spdlog/sinks/dist_sink.h"

...
auto dist_sink = make_shared<spdlog::sinks::dist_sink_st>();
auto sink1 = make_shared<spdlog::sinks::stdout_sink_st>();
auto sink2 = make_shared<spdlog::sinks::simple_file_sink_st>("mylog.log");

dist_sink->add_sink(sink1);
dist_sink->add_sink(sink2);
```

***
**msvc_sink** 
Windows debug sink (logging using OutputDebugStringA):

```c++
#include "spdlog/sinks/msvc_sink.h"
auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
auto logger = std::make_shared<spdlog::logger>("msvc_logger", sink);
```

***
**dup_filter_sink**
Duplicate messages removal sink.
Skip the message if previous one is identical and less than "max_skip_duration" have passed.

Example:
```c++
#include "spdlog/sinks/dup_filter_sink.h"

auto dup_filter = std::make_shared<dup_filter_sink_st>(std::chrono::seconds(5));
dup_filter->add_sink(std::make_shared<stdout_color_sink_mt>());
spdlog::logger l("logger", dup_filter);
l.info("Hello");
l.info("Hello");
l.info("Hello");
l.info("Different Hello");
```
Will produce:
```
[2019-06-25 17:50:56.511] [logger] [info] Hello
[2019-06-25 17:50:56.512] [logger] [info] Skipped 3 duplicate messages..
[2019-06-25 17:50:56.512] [logger] [info] Different Hello
```

***
**callback_sink**

Invokes a user-provided lambda on each log message:

```c++
#include "spdlog/sinks/callback_sink.h"

auto callback_sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
    [](const spdlog::details::log_msg &msg) {
        // handle log event (e.g., send email, update UI)
    });
callback_sink->set_level(spdlog::level::err); // only fire on errors

auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
spdlog::logger logger("callback_logger", {console_sink, callback_sink});
```

***
**ringbuffer_sink**

ringbuffer sink keeps most recent log messages in memory.
To retrieve a log message, call `spdlog::sinks::ringbuffer_sink::last_formatted(size_t)`.

Example:
```c++
#include "spdlog/sinks/ringbuffer_sink.h"

auto ringbuffer_sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(128);

std::vector<spdlog::sink_ptr> sinks;
sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("path/to/log.txt"));
sinks.push_back(ringbuffer_sink);

auto logger = std::make_shared<spdlog::logger>("logger_name", std::begin(sinks), std::end(sinks));

for (int i = 0; i < 256; ++i) {
    logger->info("Log message {}", i);
}

// Retrieve all log messages. `log_message` contains 128 messages.
std::vector<std::string> log_messages = ringbuffer_sink->last_formatted();
// Retrieve a maximum of 64 log messages.
std::vector<std::string> log_messages = ringbuffer_sink->last_formatted(64);
```
***
**qt_sink**

qt_sink could output log messages from QTextBrowser,QTextEdit...

Example:
```c++
#include "spdlog/sinks/qt_sinks.h"
auto logger = spdlog::qt_logger_mt("QLogger",ui->textBrowser);
logger->info("hello QTextBrowser");
logger->warn("this msg from spdlog");
```
![image](https://user-images.githubusercontent.com/40905056/191399533-8bcd7159-82cd-4221-9288-defcbde27df5.png)


### Implementing your own sink

To implement your own sink, you'll need to implement the simple [sink](https://github.com/gabime/spdlog/tree/v1.x/include/spdlog/sinks/sink.h) interface.

A recommended way is to inherit from the [base_sink](https://github.com/gabime/spdlog/tree/v1.x/include/spdlog/sinks/base_sink.h) class.
This class already handles thread locking, and makes it very easy to implement a thread safe sink.

In this case you only need to implement the protected "sink_it_(..)" and flush_(..) functions:
```c++
#include "spdlog/sinks/base_sink.h"

template<typename Mutex>
class my_sink : public spdlog::sinks::base_sink <Mutex>
{
...
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {

    // log_msg is a struct containing the log entry info like level, timestamp, thread id etc.
    // msg.payload (before v1.3.0: msg.raw) contains pre formatted log

    // If needed (very likely but not mandatory), the sink formats the message before sending it to its final destination:
    spdlog::memory_buf_t formatted;
    spdlog::sinks::base_sink<Mutex>::formatter_->format(msg, formatted);
    std::cout << fmt::to_string(formatted);
    }

    void flush_() override 
    {
       std::cout << std::flush;
    }
};

#include "spdlog/details/null_mutex.h"
#include <mutex>
using my_sink_mt = my_sink<std::mutex>;
using my_sink_st = my_sink<spdlog::details::null_mutex>;
```

### Adding sinks to the logger after creation:
As of spdlog v1.x there is a function returning a non-const reference to the sinks vector, allowing you to manually push back sinks. There is no mutex protecting the sinks vector (which is obvious due to the performance impact) so it's **NOT thread-safe**! Also see: https://github.com/gabime/spdlog/wiki/1.1.-Thread-Safety

```c++
inline std::vector<spdlog::sink_ptr> &spdlog::logger::sinks()
{
    return sinks_;
}
```

```c++
spdlog::get("myExistingLogger")->sinks().push_back(myNewSink);
```

## File Event Handlers

File sinks accept `spdlog::file_event_handlers` for open/close notifications:

```c++
#include "spdlog/sinks/basic_file_sink.h"

spdlog::file_event_handlers handlers;
handlers.before_open = [](spdlog::filename_t filename) {
    spdlog::info("Before opening {}", filename);
};
handlers.after_open = [](spdlog::filename_t filename, std::FILE *fstream) {
    fputs("--- Log started ---\n", fstream);
};
handlers.before_close = [](spdlog::filename_t filename, std::FILE *fstream) {
    fputs("--- Log ending ---\n", fstream);
};
handlers.after_close = [](spdlog::filename_t filename) {
    spdlog::info("After closing {}", filename);
};

auto logger = spdlog::basic_logger_st("some_logger", "logs/events.txt", true, handlers);
```