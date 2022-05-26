# Threaded Logger

## Features

* Let the writer threads as free as possible (no mutexes, limited atomics)
* Send output lines per lines in an atomic manner (human monitoring)
* The reader thread consume as less ressources as possible
* Limit the amount of memory consumed by reusing queues
* Memory allocated one time for all at thread creation time
* Easy `pthread_create` wrapper to handle internal house keeping transparently
* Easy to remove all the logging (or some levels) by changing defines
* Easy to switch between the classical full text or ansi colors
* Log levels are compatible with syslog

## Design

               +---------------+       _______       +----------+
    stdout <-- | logger thread | <--- | lines | <--- | thread 1 |
               +---------------+   |   -------       +----------+
                      ^ |          |   _______       +----------+
                      | v          -- | lines | <--- | thread 2 |
                 ____________      |   -------       +----------+
                | line thr 2 |     |   _______       +----------+
                 ------------      -- | lines | <--- | thread n |
                | line thr 1 |         -------       +----------+
                 ------------
                | line thr n |
                 ------------

Each threads have its own queue allocated when it is forked, with a fine
tunable number of lines, used to buffer the lines to output.  The logger
thread takes care of reading these lines from all the queues in a
chronological order throught an internal sorted 'fuse table', before beeing
formatted and sent to the standard output.

As there is only one reader and one writer per queue, there is no need to
use the classical locking mechanism between the threads.  This let them free
for more parallelism in multi core environments.

The queues can be finetuned when the thread is forked.  More buffer the
thread have, more burst loggings can be handled before forcing the writer
thread to wait (or loose the line if non-blocking mode is enabled).

The queue can also be allowed to loose lines, avoiding the thread to block
until some space are freeed.

For convenience / easy tracing, another option is there to log the number of
lost lines soon as some more space is free.  This allows you to know the
queue was too small at a certain point in time and maybe fine tune the
buffer size for that thread or debug why this is happening at these times.

As stated, the main goal of this logger is to minimize as much as possible
the time spent by the threads to log something on the terminal or on slow
block devices.  And also not to serialize them when logging, who is the
main problem of many logger libraries.
