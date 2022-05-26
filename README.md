# Threaded Logger

## Features

* Lockless implementation of multi-threaded logging
* Send output lines per lines in an atomic manner (human monitoring)
* The reader thread consume as less ressources as possible
* Limit the amount of memory consumed by reusing queues
* Memory allocated one time for all at thread creation time
* Able to remove all the logging (or some levels) by changing defines

## Design

               +---------------+       _______       +----------+
    stdout <-- | logger thread | <--- | lines | <--- | thread 1 |
               +---------------+   |   -------       +----------+
                      ^ |          |   _______       +----------+
                      | v          -- | lines | <--- | thread 2 |
               _______________     |   -------       +----------+
              | line thread 1 |    |   _______       +----------+
               ---------------     -- | lines | <--- | thread n |
              | line thread 2 |        -------       +----------+
               ---------------
              | line thread n |
               ---------------

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

As stated, the main goal of this logger is to minimize as much as possible
the time spent by the threads to log something on the terminal or on slow
block devices.  And also not to serialize them when logging, who is the
main problem of many logger libraries.
