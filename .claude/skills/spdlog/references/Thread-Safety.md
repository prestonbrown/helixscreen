## Non thread safe functions
The following functions **should not** be called concurrently from multiple threads on the same logger object:
* ```set_error_handler(log_err_handler);``` 
* ```logger::sinks()``` - returns a reference to a non thread safe vector, so don't modify it concurrently (e.g. ```logger->sinks().push_back(new_sink);```) 

Note: This restriction applies to all kind of loggers ("_mt" or "_st").

### Loggers
To create **thread safe** loggers, use the _mt factory functions.

For example:
```c++
auto logger = spdlog::basic_logger_mt(...);
```

To create **single threaded** loggers, use the _st factory functions.

For example:
```c++
auto logger = spdlog::basic_logger_st(...);
```

### Sinks
- **Thread safe sinks:** sinks ending with ```_mt``` (e.g ```daily_file_sink_mt```)
- **Non thread safe sinks:** sinks ending with ```_st``` (e.g ```daily_file_sink_st```)


