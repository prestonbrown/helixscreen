## Creating async loggers
There are several ways to create an asynchronous logger. In all ways you must ```#include spdlog/async.h```:


### Using the ```<spdlog::async_factory>``` template argument:
```c++
#include "spdlog/async.h"
void async_example()
{
    // default thread pool settings can be modified *before* creating the async logger:
    // spdlog::init_thread_pool(8192, 1); // queue with 8k items and 1 backing thread.
    auto async_file = spdlog::basic_logger_mt<spdlog::async_factory>("async_file_logger", "logs/async_log.txt");
}
```

### Using ```spdlog::create_async<Sink>```:
```c++
auto async_file = spdlog::create_async<spdlog::sinks::basic_file_sink_mt>("async_file_logger", "logs/async_log.txt");    
```
### Using  ```spdlog::create_async_nb<Sink>```:
This will create a logger that will never block on full queue.
```c++
auto async_file = spdlog::create_async_nb<spdlog::sinks::basic_file_sink_mt>("async_file_logger", "logs/async_log.txt");
```

### Construct directly and use the global thread pool
```c++
spdlog::init_thread_pool(queue_size, n_threads);
auto logger = std::make_shared<spdlog::async_logger>("as", some_sink, spdlog::thread_pool(), async_overflow_policy::block);
```

### Construct directly and use a custom thread pool
```c++
auto tp = std::make_shared<details::thread_pool>(queue_size, n_threads);
auto logger = std::make_shared<spdlog::async_logger>("as", some_sink, tp, async_overflow_policy::block);
```
#### Note: the tp object in the above example must outlive the logger object, since the logger acquires a weak_ptr to the tp.

## Full queue policy
There are 2 options on what to do when the queue is full:
* Block the caller until there is more room (default behaviour)
* Drop and replace the oldest messages in the queue with new ones instead of waiting for more room.
  Use the ```create_async_nb``` factory function or  the ```spdlog::async_overflow_policy``` in the logger's constructor:
```c++
auto logger = spdlog::create_async_nb<spdlog::sinks::basic_file_sink_mt>("async_file_logger", "logs/async_log.txt");
// or directly:
 auto logger = std::make_shared<async_logger>("as", test_sink, spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);
```

## spdlog's thread pool
By default, spdlog create a global thread pool with queue size of 8192 and 1 worker thread which to serve **all** async loggers. 

This means that creating and destructing async loggers is cheap, since they do not own nor create any backing threads or queues- these are created and managed by the shared thread pool object.

All the queue slots are **pre-allocated** on thread-pool construction (each slot occupies ~256 bytes on 64bit systems)

The thread pool size and threads can be reset by
```c++
spdlog::init_thread_pool(queue_size, n_threads);
```
Note that this will drop the old global thread pool (tp) and create a new tp - which means that any loggers that use the old tp will stop working, so it is advised calling it before any async loggers created.

If different loggers must have separate queues, different instances of pools can be created and passed them to the loggers:

```c++
auto tp = std::make_shared<details::thread_pool>(128, 1);
auto logger = std::make_shared<async_logger>("as", some_sink, tp, async_overflow_policy::overrun_oldest);

auto tp2 = std::make_shared<details::thread_pool>(1024, 4);  // create pool with queue of 1024 slots and 4 backing threads
auto logger2 = std::make_shared<async_logger>("as2", some_sink, tp2, async_overflow_policy::block);
```

Thread-pool has additional constructors that take callbacks as arguments. Those will be executed by each thread following its creation and prior to its destruction.
```c++
std::function<void()> on_start = []() { /* execute on start */ };
auto on_stop = []() { /* execute on stop */ };
auto tp = std::make_shared<details::thread_pool>(1024, 1, on_start, on_stop);

// init_thread_pool helper also supports optional on_start and on_stop callbacks
spdlog::init_thread_pool(1024, 1, on_start);
spdlog::init_thread_pool(1024, 1, on_start, on_stop);
```

### Message ordering
By default, spdlog creates a single worker thread which preserves the order of the messages in the queue.

Please note that with thread pool with multiple workers threads, messages may be reordered after dequeuing.

If you want to keep the message order, create only one worker thread in the thread pool.

#### Windows issues
There is a bug in VS runtime that cause the application dead lock when it exits. If you use async logging, please make sure to call ```spdlog::shutdown()``` before main() exit
([stackoverflow: std::thread join hangs if called after main exits when using vs2012 rc ](http://stackoverflow.com/questions/10915233/stdthreadjoin-hangs-if-called-after-main-exits-when-using-vs2012-)).